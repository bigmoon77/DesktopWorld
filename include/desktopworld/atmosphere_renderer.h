#pragma once

#include "desktopworld/types.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace desktopworld
{
    class atmosphere_renderer
    {
    public:
        static constexpr unsigned int sky_width = 256;
        static constexpr unsigned int sky_height = 128;

        bool initialize(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            const wchar_t* shader_path);

        void shutdown();

        bool set_atmosphere_parameters(const atmosphere_parameters& parameters);
        void set_exposure(float exposure);
        void update_current_sky(const solar_state& current);
        void update_sky_samples(
            const solar_state& sample_a,
            const solar_state& sample_b,
            const solar_state& current,
            float blend_factor);

        void draw(
            ID3D11RenderTargetView* target,
            unsigned int width,
            unsigned int height,
            const solar_state& current_sun,
            float blend_factor);

        void set_render_method(render_method method);
        render_method get_render_method() const;

        const atmosphere_parameters& get_parameters() const;
        bool is_initialized() const;

    private:
        struct frame_constants
        {
            float resolution[2]{};
            float inverse_resolution[2]{};
            float sun_direction[3]{};
            float exposure = 1.0f;
            float blend_factor = 0.0f;
            float padding[3]{};
        };

        struct atmosphere_constants
        {
            float rayleigh_scattering[3]{};
            float planet_radius_km = 6360.0f;
            float mie_scattering[3]{};
            float atmosphere_height_km = 100.0f;
            float rayleigh_scale_height_km = 8.0f;
            float mie_scale_height_km = 1.2f;
            float mie_g = 0.8f;
            float solar_intensity = 20.0f;
        };

        bool create_shaders(const wchar_t* shader_path);
        bool create_constant_buffers();
        bool create_textures();
        bool create_render_target(
            unsigned int width,
            unsigned int height,
            ID3D11Texture2D** texture,
            ID3D11ShaderResourceView** srv,
            ID3D11UnorderedAccessView** uav);
        bool compile_shader(
            const wchar_t* path,
            const char* entry_point,
            const char* target,
            ID3DBlob** bytecode);

        void update_constant_buffers(const solar_state& sun, float blend_factor);
        void generate_transmittance();
        void generate_sky_view(
            ID3D11UnorderedAccessView* target,
            const solar_state& sun);
        void blend_sky_views(float blend_factor);
        void clear_compute_bindings();

        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context;

        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _transmittance_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _sky_view_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _blend_cs;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _fullscreen_vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _direct_ps;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _current_ps;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pair_ps;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _final_ps;

        Microsoft::WRL::ComPtr<ID3D11SamplerState> _linear_sampler;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _clamp_sampler;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer_state;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _frame_cb;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _atmosphere_cb;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _transmittance_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _transmittance_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _transmittance_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _sky_a_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sky_a_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _sky_a_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _sky_b_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sky_b_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _sky_b_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _sky_current_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sky_current_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _sky_current_uav;

        atmosphere_parameters _parameters{};
        render_method _render_method = render_method::sky_blend_compute;
        bool _initialized = false;
    };
}
