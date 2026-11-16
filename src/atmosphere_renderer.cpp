#include "desktopworld/atmosphere_renderer.h"

#include <d3dcompiler.h>
#include <filesystem>
#include <string>

namespace desktopworld
{
    namespace
    {
        constexpr unsigned int transmittance_width = 256;
        constexpr unsigned int transmittance_height = 64;
        constexpr DXGI_FORMAT lut_format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        template<typename T>
        void set_float3(float (&destination)[3], const T& source)
        {
            destination[0] = source.x;
            destination[1] = source.y;
            destination[2] = source.z;
        }
    }

    bool atmosphere_renderer::initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        const wchar_t* shader_path)
    {
        if (device == nullptr || context == nullptr || shader_path == nullptr)
        {
            return false;
        }

        _device = device;
        _context = context;

        if (!create_shaders(shader_path) ||
            !create_constant_buffers() ||
            !create_textures())
        {
            shutdown();
            return false;
        }

        _initialized = true;
        set_atmosphere_parameters(_parameters);
        return true;
    }

    void atmosphere_renderer::shutdown()
    {
        if (_context)
        {
            _context->ClearState();
        }

        _sky_current_uav.Reset();
        _sky_current_srv.Reset();
        _sky_current_texture.Reset();
        _sky_b_uav.Reset();
        _sky_b_srv.Reset();
        _sky_b_texture.Reset();
        _sky_a_uav.Reset();
        _sky_a_srv.Reset();
        _sky_a_texture.Reset();
        _transmittance_uav.Reset();
        _transmittance_srv.Reset();
        _transmittance_texture.Reset();

        _linear_sampler.Reset();
        _linear_sampler.Reset();
        _frame_cb.Reset();
        _atmosphere_cb.Reset();

        _transmittance_cs.Reset();
        _sky_view_cs.Reset();
        _blend_cs.Reset();
        _fullscreen_vs.Reset();
        _direct_ps.Reset();
        _current_ps.Reset();
        _pair_ps.Reset();
        _final_ps.Reset();

        _context.Reset();
        _device.Reset();
        _initialized = false;
    }

    bool atmosphere_renderer::set_atmosphere_parameters(
        const atmosphere_parameters& parameters)
    {
        if (!_device || !_context || !_atmosphere_cb)
        {
            _parameters = parameters;
            return false;
        }

        _parameters = parameters;

        atmosphere_constants constants{};
        set_float3(constants.rayleigh_scattering, parameters.rayleigh_scattering);
        constants.planet_radius_km = parameters.planet_radius_km;
        set_float3(constants.mie_scattering, parameters.mie_scattering);
        constants.atmosphere_height_km = parameters.atmosphere_height_km;
        constants.rayleigh_scale_height_km = parameters.rayleigh_scale_height_km;
        constants.mie_scale_height_km = parameters.mie_scale_height_km;
        constants.mie_g = parameters.mie_g;
        constants.solar_intensity = parameters.solar_intensity;

        _context->UpdateSubresource(_atmosphere_cb.Get(), 0, nullptr, &constants, 0, 0);
        generate_transmittance();
        return true;
    }

    void atmosphere_renderer::set_exposure(const float exposure)
    {
        _parameters.exposure = clamp_float(exposure, 0.01f, 16.0f);
    }

    void atmosphere_renderer::update_current_sky(const solar_state& current)
    {
        if (_initialized && _render_method == render_method::sky_current)
        {
            generate_sky_view(_sky_current_uav.Get(), current);
        }
    }

    void atmosphere_renderer::update_sky_samples(
        const solar_state& sample_a,
        const solar_state& sample_b,
        const solar_state& current,
        const float blend_factor)
    {
        if (!_initialized)
        {
            return;
        }

        if (_render_method == render_method::sky_current)
        {
            generate_sky_view(_sky_current_uav.Get(), current);
        }
        else if (_render_method == render_method::sky_blend_pixel ||
                 _render_method == render_method::sky_blend_compute)
        {
            generate_sky_view(_sky_a_uav.Get(), sample_a);
            generate_sky_view(_sky_b_uav.Get(), sample_b);
        }
    }

    void atmosphere_renderer::draw(
        ID3D11RenderTargetView* target,
        const unsigned int width,
        const unsigned int height,
        const solar_state& current_sun,
        const float blend_factor)
    {
        if (!_initialized || target == nullptr || width == 0 || height == 0)
        {
            return;
        }

        if (_render_method == render_method::sky_blend_compute)
        {
            blend_sky_views(blend_factor);
        }

        update_constant_buffers(current_sun, blend_factor);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        _context->OMSetRenderTargets(1, &target, nullptr);
        _context->RSSetViewports(1, &viewport);
        _context->IASetInputLayout(nullptr);
        _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _context->VSSetShader(_fullscreen_vs.Get(), nullptr, 0);

        ID3D11Buffer* constant_buffers[] = { _frame_cb.Get(), _atmosphere_cb.Get() };
        _context->PSSetConstantBuffers(0, 2, constant_buffers);
        ID3D11SamplerState* sampler = _linear_sampler.Get();
        _context->PSSetSamplers(0, 1, &sampler);

        ID3D11ShaderResourceView* resources[] =
        {
            _transmittance_srv.Get(),
            _sky_a_srv.Get(),
            _sky_b_srv.Get(),
            _sky_current_srv.Get()
        };
        _context->PSSetShaderResources(0, 4, resources);

        ID3D11PixelShader* pixel_shader = _direct_ps.Get();
        switch (_render_method)
        {
        case render_method::direct:
            pixel_shader = _direct_ps.Get();
            break;
        case render_method::sky_current:
            pixel_shader = _current_ps.Get();
            break;
        case render_method::sky_blend_pixel:
            pixel_shader = _pair_ps.Get();
            break;
        case render_method::sky_blend_compute:
            pixel_shader = _final_ps.Get();
            break;
        }

        _context->PSSetShader(pixel_shader, nullptr, 0);
        _context->Draw(3, 0);

        ID3D11ShaderResourceView* null_resources[4]{};
        _context->PSSetShaderResources(0, 4, null_resources);
        _context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    void atmosphere_renderer::set_render_method(const render_method method)
    {
        _render_method = method;
    }

    render_method atmosphere_renderer::get_render_method() const
    {
        return _render_method;
    }

    const atmosphere_parameters& atmosphere_renderer::get_parameters() const
    {
        return _parameters;
    }

    bool atmosphere_renderer::is_initialized() const
    {
        return _initialized;
    }

    bool atmosphere_renderer::create_shaders(const wchar_t* shader_path)
    {
        const std::filesystem::path path =
            std::filesystem::path(shader_path) / L"atmosphere.hlsl";

        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;

        const auto create_compute = [&](const char* entry, ID3D11ComputeShader** shader) -> bool
        {
            if (!compile_shader(path.c_str(), entry, "cs_5_0", bytecode.GetAddressOf()))
            {
                return false;
            }
            return SUCCEEDED(_device->CreateComputeShader(
                bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(),
                nullptr,
                shader));
        };

        const auto create_pixel = [&](const char* entry, ID3D11PixelShader** shader) -> bool
        {
            if (!compile_shader(path.c_str(), entry, "ps_5_0", bytecode.GetAddressOf()))
            {
                return false;
            }
            return SUCCEEDED(_device->CreatePixelShader(
                bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(),
                nullptr,
                shader));
        };

        if (!create_compute("CS_Transmittance", _transmittance_cs.GetAddressOf()) ||
            !create_compute("CS_SkyView", _sky_view_cs.GetAddressOf()) ||
            !create_compute("CS_Blend", _blend_cs.GetAddressOf()))
        {
            return false;
        }

        if (!compile_shader(path.c_str(), "VS_Fullscreen", "vs_5_0", bytecode.GetAddressOf()) ||
            FAILED(_device->CreateVertexShader(
                bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(),
                nullptr,
                _fullscreen_vs.GetAddressOf())))
        {
            return false;
        }

        if (!create_pixel("PS_Direct", _direct_ps.GetAddressOf()) ||
            !create_pixel("PS_SkyCurrent", _current_ps.GetAddressOf()) ||
            !create_pixel("PS_SkyPair", _pair_ps.GetAddressOf()) ||
            !create_pixel("PS_SkyFinal", _final_ps.GetAddressOf()))
        {
            return false;
        }

        D3D11_SAMPLER_DESC sampler_description{};
        sampler_description.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler_description.MinLOD = 0.0f;
        sampler_description.MaxLOD = D3D11_FLOAT32_MAX;
        return SUCCEEDED(_device->CreateSamplerState(
            &sampler_description,
            _linear_sampler.GetAddressOf()));
    }

    bool atmosphere_renderer::create_constant_buffers()
    {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(frame_constants);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        if (FAILED(_device->CreateBuffer(
            &description, nullptr, _frame_cb.GetAddressOf())))
        {
            return false;
        }

        description.ByteWidth = sizeof(atmosphere_constants);
        return SUCCEEDED(_device->CreateBuffer(
            &description, nullptr, _atmosphere_cb.GetAddressOf()));
    }

    bool atmosphere_renderer::create_textures()
    {
        return create_render_target(
                transmittance_width,
                transmittance_height,
                _transmittance_texture.GetAddressOf(),
                _transmittance_srv.GetAddressOf(),
                _transmittance_uav.GetAddressOf()) &&
            create_render_target(
                sky_width,
                sky_height,
                _sky_a_texture.GetAddressOf(),
                _sky_a_srv.GetAddressOf(),
                _sky_a_uav.GetAddressOf()) &&
            create_render_target(
                sky_width,
                sky_height,
                _sky_b_texture.GetAddressOf(),
                _sky_b_srv.GetAddressOf(),
                _sky_b_uav.GetAddressOf()) &&
            create_render_target(
                sky_width,
                sky_height,
                _sky_current_texture.GetAddressOf(),
                _sky_current_srv.GetAddressOf(),
                _sky_current_uav.GetAddressOf());
    }

    bool atmosphere_renderer::create_render_target(
        const unsigned int width,
        const unsigned int height,
        ID3D11Texture2D** texture,
        ID3D11ShaderResourceView** srv,
        ID3D11UnorderedAccessView** uav)
    {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width;
        description.Height = height;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = lut_format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        if (FAILED(_device->CreateTexture2D(&description, nullptr, texture)))
        {
            return false;
        }
        if (FAILED(_device->CreateShaderResourceView(*texture, nullptr, srv)))
        {
            return false;
        }
        return SUCCEEDED(_device->CreateUnorderedAccessView(*texture, nullptr, uav));
    }

    bool atmosphere_renderer::compile_shader(
        const wchar_t* path,
        const char* entry_point,
        const char* target,
        ID3DBlob** bytecode)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompileFromFile(
            path,
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry_point,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS,
#ifdef _DEBUG
            D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
#else
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
#endif
            bytecode,
            errors.GetAddressOf());

        if (FAILED(result))
        {
            if (errors)
            {
                OutputDebugStringA(
                    static_cast<const char*>(errors->GetBufferPointer()));
            }
            return false;
        }
        return true;
    }

    void atmosphere_renderer::update_constant_buffers(
        const solar_state& sun,
        const float blend_factor)
    {
        frame_constants constants{};
        constants.sun_direction[0] = sun.direction.x;
        constants.sun_direction[1] = sun.direction.y;
        constants.sun_direction[2] = sun.direction.z;
        constants.exposure = _parameters.exposure;
        constants.blend_factor = blend_factor;

        _context->UpdateSubresource(_frame_cb.Get(), 0, nullptr, &constants, 0, 0);
        ID3D11Buffer* buffers[] = { _frame_cb.Get(), _atmosphere_cb.Get() };
        _context->CSSetConstantBuffers(0, 2, buffers);
    }

    void atmosphere_renderer::generate_transmittance()
    {
        if (!_transmittance_cs || !_transmittance_uav)
        {
            return;
        }

        update_constant_buffers(solar_state{}, 0.0f);
        ID3D11UnorderedAccessView* output = _transmittance_uav.Get();
        _context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        _context->CSSetShader(_transmittance_cs.Get(), nullptr, 0);
        _context->Dispatch(
            (transmittance_width + 7) / 8,
            (transmittance_height + 7) / 8,
            1);
        clear_compute_bindings();
    }

    void atmosphere_renderer::generate_sky_view(
        ID3D11UnorderedAccessView* target,
        const solar_state& sun)
    {
        if (target == nullptr)
        {
            return;
        }

        update_constant_buffers(sun, 0.0f);
        ID3D11ShaderResourceView* input = _transmittance_srv.Get();
        _context->CSSetShaderResources(0, 1, &input);
        ID3D11SamplerState* sampler = _linear_sampler.Get();
        _context->CSSetSamplers(0, 1, &sampler);
        ID3D11SamplerState* sampler = _linear_sampler.Get();
        _context->CSSetSamplers(0, 1, &sampler);
        _context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        _context->CSSetShader(_sky_view_cs.Get(), nullptr, 0);
        _context->Dispatch((sky_width + 7) / 8, (sky_height + 7) / 8, 1);
        clear_compute_bindings();
    }

    void atmosphere_renderer::blend_sky_views(const float blend_factor)
    {
        update_constant_buffers(solar_state{}, blend_factor);

        ID3D11ShaderResourceView* inputs[] =
        {
            _sky_a_srv.Get(),
            _sky_b_srv.Get()
        };
        _context->CSSetShaderResources(1, 2, inputs);
        ID3D11UnorderedAccessView* output = _sky_current_uav.Get();
        _context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        _context->CSSetShader(_blend_cs.Get(), nullptr, 0);
        _context->Dispatch((sky_width + 7) / 8, (sky_height + 7) / 8, 1);
        clear_compute_bindings();
    }

    void atmosphere_renderer::clear_compute_bindings()
    {
        ID3D11ShaderResourceView* null_srvs[4]{};
        ID3D11UnorderedAccessView* null_uavs[1]{};
        _context->CSSetShaderResources(0, 4, null_srvs);
        _context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
        _context->CSSetShader(nullptr, nullptr, 0);
    }
}
