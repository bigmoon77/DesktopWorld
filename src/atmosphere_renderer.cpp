#include "desktopworld/atmosphere_renderer.h"

#include <d3dcompiler.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace desktopworld
{
    namespace
    {
        constexpr unsigned int transmittance_width = 256;
        constexpr unsigned int transmittance_height = 64;
        constexpr DXGI_FORMAT lut_format = DXGI_FORMAT_R16G16B16A16_FLOAT;

        void set_float3(float (&destination)[3], const DirectX::XMFLOAT3& source)
        {
            destination[0] = source.x;
            destination[1] = source.y;
            destination[2] = source.z;
        }

        std::uint64_t update_hash(
            std::uint64_t hash,
            const void* value,
            const std::size_t byte_count)
        {
            constexpr std::uint64_t prime = 1099511628211ULL;
            const unsigned char* bytes =
                static_cast<const unsigned char*>(value);

            for (std::size_t index = 0; index < byte_count; ++index)
            {
                hash ^= bytes[index];
                hash *= prime;
            }

            return hash;
        }

        std::uint64_t calculate_parameter_hash(const atmosphere_parameters& parameters)
        {
            constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
            std::uint64_t hash = offset_basis;

            hash = update_hash(hash, &parameters.planet_radius_km, sizeof(float));
            hash = update_hash(hash, &parameters.atmosphere_height_km, sizeof(float));
            hash = update_hash(hash, &parameters.rayleigh_scattering, sizeof(parameters.rayleigh_scattering));
            hash = update_hash(hash, &parameters.rayleigh_scale_height_km, sizeof(float));
            hash = update_hash(hash, &parameters.mie_scattering, sizeof(parameters.mie_scattering));
            hash = update_hash(hash, &parameters.mie_extinction, sizeof(parameters.mie_extinction));
            hash = update_hash(hash, &parameters.mie_scale_height_km, sizeof(float));
            hash = update_hash(hash, &parameters.mie_g, sizeof(float));
            hash = update_hash(hash, &parameters.ozone_absorption, sizeof(parameters.ozone_absorption));
            hash = update_hash(hash, &parameters.ozone_center_height_km, sizeof(float));
            hash = update_hash(hash, &parameters.ozone_half_width_km, sizeof(float));
            hash = update_hash(hash, &parameters.ground_albedo, sizeof(parameters.ground_albedo));
            hash = update_hash(hash, &parameters.solar_intensity, sizeof(float));
            hash = update_hash(hash, &parameters.max_scattering_order, sizeof(std::uint32_t));

            const std::uint32_t format =
                static_cast<std::uint32_t>(lut_format);
            hash = update_hash(hash, &format, sizeof(format));
            hash = update_hash(hash, &atmosphere_renderer::irradiance_width, sizeof(atmosphere_renderer::irradiance_width));
            hash = update_hash(hash, &atmosphere_renderer::irradiance_height, sizeof(atmosphere_renderer::irradiance_height));
            hash = update_hash(hash, &atmosphere_renderer::scattering_height, sizeof(atmosphere_renderer::scattering_height));
            hash = update_hash(hash, &atmosphere_renderer::scattering_depth, sizeof(atmosphere_renderer::scattering_depth));

            constexpr std::uint32_t algorithm_version = 3;
            hash = update_hash(hash, &algorithm_version, sizeof(algorithm_version));
            hash = update_hash(hash, &transmittance_width, sizeof(transmittance_width));
            hash = update_hash(hash, &transmittance_height, sizeof(transmittance_height));
            hash = update_hash(hash, &atmosphere_renderer::scattering_width,
                sizeof(atmosphere_renderer::scattering_width));

            return hash;
        }


        struct static_cache_header
        {
            char magic[8]{};
            std::uint32_t version = 1;
            std::uint32_t format = static_cast<std::uint32_t>(lut_format);
            std::uint32_t transmittance_width = 0;
            std::uint32_t transmittance_height = 0;
            std::uint32_t irradiance_width = 0;
            std::uint32_t irradiance_height = 0;
            std::uint32_t scattering_width = 0;
            std::uint32_t scattering_height = 0;
            std::uint32_t scattering_depth = 0;
            std::uint64_t parameter_hash = 0;
        };

        bool write_texture_2d(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            ID3D11Texture2D* texture,
            std::ostream& stream)
        {
            if (device == nullptr || context == nullptr || texture == nullptr)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);

            if (description.Format != lut_format ||
                description.SampleDesc.Count != 1)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC staging_description = description;
            staging_description.Usage = D3D11_USAGE_STAGING;
            staging_description.BindFlags = 0;
            staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_description.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(
                &staging_description,
                nullptr,
                staging.GetAddressOf())))
            {
                return false;
            }

            context->CopyResource(staging.Get(), texture);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(
                staging.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped)))
            {
                return false;
            }

            const std::size_t row_bytes =
                static_cast<std::size_t>(description.Width) * 8U;

            bool success = true;
            for (std::uint32_t row = 0; row < description.Height; ++row)
            {
                const char* source =
                    static_cast<const char*>(mapped.pData) +
                    static_cast<std::size_t>(row) * mapped.RowPitch;
                stream.write(source, static_cast<std::streamsize>(row_bytes));
                if (!stream.good())
                {
                    success = false;
                    break;
                }
            }

            context->Unmap(staging.Get(), 0);
            return success;
        }

        bool read_texture_2d(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            ID3D11Texture2D* texture,
            std::istream& stream)
        {
            if (device == nullptr || context == nullptr || texture == nullptr)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);

            if (description.Format != lut_format ||
                description.SampleDesc.Count != 1)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC staging_description = description;
            staging_description.Usage = D3D11_USAGE_STAGING;
            staging_description.BindFlags = 0;
            staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            staging_description.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(
                &staging_description,
                nullptr,
                staging.GetAddressOf())))
            {
                return false;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(
                staging.Get(),
                0,
                D3D11_MAP_WRITE,
                0,
                &mapped)))
            {
                return false;
            }

            const std::size_t row_bytes =
                static_cast<std::size_t>(description.Width) * 8U;

            bool success = true;
            for (std::uint32_t row = 0; row < description.Height; ++row)
            {
                char* destination =
                    static_cast<char*>(mapped.pData) +
                    static_cast<std::size_t>(row) * mapped.RowPitch;
                stream.read(destination, static_cast<std::streamsize>(row_bytes));
                if (!stream.good())
                {
                    success = false;
                    break;
                }
            }

            context->Unmap(staging.Get(), 0);
            if (!success)
            {
                return false;
            }

            context->CopyResource(texture, staging.Get());
            return true;
        }

        bool write_texture_3d(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            ID3D11Texture3D* texture,
            std::ostream& stream)
        {
            if (device == nullptr || context == nullptr || texture == nullptr)
            {
                return false;
            }

            D3D11_TEXTURE3D_DESC description{};
            texture->GetDesc(&description);

            if (description.Format != lut_format)
            {
                return false;
            }

            D3D11_TEXTURE3D_DESC staging_description = description;
            staging_description.Usage = D3D11_USAGE_STAGING;
            staging_description.BindFlags = 0;
            staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_description.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture3D> staging;
            if (FAILED(device->CreateTexture3D(
                &staging_description,
                nullptr,
                staging.GetAddressOf())))
            {
                return false;
            }

            context->CopyResource(staging.Get(), texture);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(
                staging.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped)))
            {
                return false;
            }

            const std::size_t row_bytes =
                static_cast<std::size_t>(description.Width) * 8U;

            bool success = true;
            for (std::uint32_t depth = 0; depth < description.Depth; ++depth)
            {
                const char* depth_data =
                    static_cast<const char*>(mapped.pData) +
                    static_cast<std::size_t>(depth) * mapped.DepthPitch;

                for (std::uint32_t row = 0; row < description.Height; ++row)
                {
                    const char* source =
                        depth_data +
                        static_cast<std::size_t>(row) * mapped.RowPitch;
                    stream.write(source, static_cast<std::streamsize>(row_bytes));
                    if (!stream.good())
                    {
                        success = false;
                        break;
                    }
                }

                if (!success)
                {
                    break;
                }
            }

            context->Unmap(staging.Get(), 0);
            return success;
        }

        bool read_texture_3d(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            ID3D11Texture3D* texture,
            std::istream& stream)
        {
            if (device == nullptr || context == nullptr || texture == nullptr)
            {
                return false;
            }

            D3D11_TEXTURE3D_DESC description{};
            texture->GetDesc(&description);

            if (description.Format != lut_format)
            {
                return false;
            }

            D3D11_TEXTURE3D_DESC staging_description = description;
            staging_description.Usage = D3D11_USAGE_STAGING;
            staging_description.BindFlags = 0;
            staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            staging_description.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture3D> staging;
            if (FAILED(device->CreateTexture3D(
                &staging_description,
                nullptr,
                staging.GetAddressOf())))
            {
                return false;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(
                staging.Get(),
                0,
                D3D11_MAP_WRITE,
                0,
                &mapped)))
            {
                return false;
            }

            const std::size_t row_bytes =
                static_cast<std::size_t>(description.Width) * 8U;

            bool success = true;
            for (std::uint32_t depth = 0; depth < description.Depth; ++depth)
            {
                char* depth_data =
                    static_cast<char*>(mapped.pData) +
                    static_cast<std::size_t>(depth) * mapped.DepthPitch;

                for (std::uint32_t row = 0; row < description.Height; ++row)
                {
                    char* destination =
                        depth_data +
                        static_cast<std::size_t>(row) * mapped.RowPitch;
                    stream.read(destination, static_cast<std::streamsize>(row_bytes));
                    if (!stream.good())
                    {
                        success = false;
                        break;
                    }
                }

                if (!success)
                {
                    break;
                }
            }

            context->Unmap(staging.Get(), 0);
            if (!success)
            {
                return false;
            }

            context->CopyResource(texture, staging.Get());
            return true;
        }

        std::filesystem::path get_static_cache_path(const std::uint64_t parameter_hash)
        {
            wchar_t local_app_data[MAX_PATH]{};
            const HRESULT result = SHGetFolderPathW(
                nullptr,
                CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                nullptr,
                SHGFP_TYPE_CURRENT,
                local_app_data);

            if (FAILED(result))
            {
                return {};
            }

            const std::filesystem::path cache_directory =
                std::filesystem::path(local_app_data) /
                L"DesktopWorld" /
                L"Cache";

            std::error_code error;
            std::filesystem::create_directories(cache_directory, error);
            if (error)
            {
                return {};
            }

            std::wostringstream file_name;
            file_name << std::hex << std::setfill(L'0')
                << std::setw(16) << parameter_hash << L".bin";

            return cache_directory / file_name.str();
        }


        float half_to_float(const std::uint16_t value)
        {
            const std::uint32_t sign =
                static_cast<std::uint32_t>(value & 0x8000U) << 16U;
            std::uint32_t exponent = (value >> 10U) & 0x1FU;
            std::uint32_t mantissa = value & 0x03FFU;
            std::uint32_t bits = sign;

            if (exponent == 0)
            {
                if (mantissa != 0)
                {
                    int normalized_exponent = -14;
                    while ((mantissa & 0x0400U) == 0)
                    {
                        mantissa <<= 1U;
                        --normalized_exponent;
                    }

                    mantissa &= 0x03FFU;
                    const std::uint32_t float_exponent =
                        static_cast<std::uint32_t>(normalized_exponent + 127);
                    bits |= (float_exponent << 23U) | (mantissa << 13U);
                }
            }
            else if (exponent == 0x1FU)
            {
                bits |= 0x7F800000U | (mantissa << 13U);
            }
            else
            {
                exponent += 112U;
                bits |= (exponent << 23U) | (mantissa << 13U);
            }

            return std::bit_cast<float>(bits);
        }

        bool read_texture_rgb16f_pixels(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            ID3D11Texture2D* texture,
            std::vector<float>& pixels,
            unsigned int& width,
            unsigned int& height)
        {
            if (device == nullptr || context == nullptr || texture == nullptr)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC description{};
            texture->GetDesc(&description);
            if (description.Format != lut_format ||
                description.SampleDesc.Count != 1 ||
                description.Width == 0 ||
                description.Height == 0)
            {
                return false;
            }

            D3D11_TEXTURE2D_DESC staging_description = description;
            staging_description.Usage = D3D11_USAGE_STAGING;
            staging_description.BindFlags = 0;
            staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_description.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(
                &staging_description,
                nullptr,
                staging.GetAddressOf())))
            {
                return false;
            }

            context->CopyResource(staging.Get(), texture);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(
                staging.Get(),
                0,
                D3D11_MAP_READ,
                0,
                &mapped)))
            {
                return false;
            }

            const std::size_t pixel_count =
                static_cast<std::size_t>(description.Width) *
                static_cast<std::size_t>(description.Height);
            std::vector<float> output(pixel_count * 3U);

            for (std::uint32_t y = 0; y < description.Height; ++y)
            {
                const auto* row = reinterpret_cast<const std::uint16_t*>(
                    static_cast<const unsigned char*>(mapped.pData) +
                    static_cast<std::size_t>(y) * mapped.RowPitch);

                for (std::uint32_t x = 0; x < description.Width; ++x)
                {
                    const std::size_t source_index =
                        static_cast<std::size_t>(x) * 4U;
                    const std::size_t destination_index =
                        (static_cast<std::size_t>(y) * description.Width + x) * 3U;

                    output[destination_index] = half_to_float(row[source_index]);
                    output[destination_index + 1U] = half_to_float(row[source_index + 1U]);
                    output[destination_index + 2U] = half_to_float(row[source_index + 2U]);
                }
            }

            context->Unmap(staging.Get(), 0);

            pixels = std::move(output);
            width = description.Width;
            height = description.Height;
            return true;
        }

        bool calculate_quality_metrics(
            const std::vector<float>& reference,
            const std::vector<float>& candidate,
            const unsigned int width,
            const unsigned int height,
            sky_quality_metrics& result)
        {
            if (width == 0 || height == 0 ||
                reference.empty() ||
                reference.size() != candidate.size() ||
                reference.size() % 3U != 0)
            {
                return false;
            }

            double absolute_error_sum = 0.0;
            double squared_error_sum = 0.0;
            double maximum_absolute_error = 0.0;

            for (std::size_t index = 0; index < reference.size(); ++index)
            {
                const float reference_value = reference[index];
                const float candidate_value = candidate[index];
                if (!std::isfinite(reference_value) ||
                    !std::isfinite(candidate_value))
                {
                    return false;
                }

                const double difference =
                    std::abs(static_cast<double>(reference_value) -
                        static_cast<double>(candidate_value));
                absolute_error_sum += difference;
                squared_error_sum += difference * difference;
                maximum_absolute_error =
                    std::max(maximum_absolute_error, difference);
            }

            const double component_count =
                static_cast<double>(reference.size());
            result.mean_absolute_error =
                absolute_error_sum / component_count;
            result.root_mean_squared_error =
                std::sqrt(squared_error_sum / component_count);
            result.maximum_absolute_error = maximum_absolute_error;
            result.pixel_count =
                static_cast<std::uint64_t>(width) * height;
            result.width = width;
            result.height = height;
            result.valid = true;
            return true;
        }

        void swap_com_ptr(
            Microsoft::WRL::ComPtr<ID3D11Texture2D>& a,
            Microsoft::WRL::ComPtr<ID3D11Texture2D>& b)
        {
            a.Swap(b);
        }

        void swap_com_ptr(
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& a,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& b)
        {
            a.Swap(b);
        }

        void swap_com_ptr(
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& a,
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& b)
        {
            a.Swap(b);
        }

        void swap_com_ptr(
            Microsoft::WRL::ComPtr<ID3D11Texture3D>& a,
            Microsoft::WRL::ComPtr<ID3D11Texture3D>& b)
        {
            a.Swap(b);
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
        _profiler.initialize(device, context);

        if (!create_shaders(shader_path) ||
            !create_constant_buffers() ||
            !create_textures())
        {
            shutdown();
            return false;
        }

        _initialized = true;
        return true;
    }

    void atmosphere_renderer::shutdown()
    {
        _profiler.shutdown();

        if (_context)
        {
            _context->ClearState();
        }

        _quality_reference_uav.Reset();
        _quality_reference_srv.Reset();
        _quality_reference_texture.Reset();

        _sky_current_uav.Reset();
        _sky_current_srv.Reset();
        _sky_current_texture.Reset();
        _sky_b_uav.Reset();
        _sky_b_srv.Reset();
        _sky_b_texture.Reset();
        _sky_a_uav.Reset();
        _sky_a_srv.Reset();
        _sky_a_texture.Reset();

        _irradiance_sum_b_uav.Reset();
        _irradiance_sum_b_srv.Reset();
        _irradiance_sum_b_texture.Reset();
        _irradiance_sum_a_uav.Reset();
        _irradiance_sum_a_srv.Reset();
        _irradiance_sum_a_texture.Reset();
        _next_irradiance_uav.Reset();
        _next_irradiance_srv.Reset();
        _next_irradiance_texture.Reset();
        _delta_irradiance_uav.Reset();
        _delta_irradiance_srv.Reset();
        _delta_irradiance_texture.Reset();
        _direct_irradiance_uav.Reset();
        _direct_irradiance_srv.Reset();
        _direct_irradiance_texture.Reset();

        _delta_multiple_uav.Reset();
        _delta_multiple_srv.Reset();
        _delta_multiple_texture.Reset();
        _scattering_density_uav.Reset();
        _scattering_density_srv.Reset();
        _scattering_density_texture.Reset();
        _scattering_sum_b_uav.Reset();
        _scattering_sum_b_srv.Reset();
        _scattering_sum_b_texture.Reset();
        _scattering_sum_a_uav.Reset();
        _scattering_sum_a_srv.Reset();
        _scattering_sum_a_texture.Reset();
        _single_mie_uav.Reset();
        _single_mie_srv.Reset();
        _single_mie_texture.Reset();
        _single_rayleigh_uav.Reset();
        _single_rayleigh_srv.Reset();
        _single_rayleigh_texture.Reset();

        _transmittance_uav.Reset();
        _transmittance_srv.Reset();
        _transmittance_texture.Reset();

        _rasterizer_state.Reset();
        _clamp_sampler.Reset();
        _linear_sampler.Reset();
        _frame_cb.Reset();
        _atmosphere_cb.Reset();

        _transmittance_cs.Reset();
        _direct_irradiance_cs.Reset();
        _single_scattering_cs.Reset();
        _scattering_density_cs.Reset();
        _multiple_scattering_cs.Reset();
        _indirect_irradiance_cs.Reset();
        _accumulate_scattering_cs.Reset();
        _accumulate_irradiance_cs.Reset();
        _sky_view_cs.Reset();
        _blend_cs.Reset();
        _fullscreen_vs.Reset();
        _direct_ps.Reset();
        _current_ps.Reset();
        _pair_ps.Reset();
        _final_ps.Reset();
        _direct_radiance_ps.Reset();
        _final_radiance_ps.Reset();

        _context.Reset();
        _device.Reset();
        _static_luts_valid = false;
        _static_parameter_hash = 0;
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

        atmosphere_parameters validated = parameters;
        validated.planet_radius_km = clamp_float(
            validated.planet_radius_km,
            1000.0f,
            10000.0f);
        validated.atmosphere_height_km = clamp_float(
            validated.atmosphere_height_km,
            1.0f,
            500.0f);
        validated.rayleigh_scale_height_km = clamp_float(
            validated.rayleigh_scale_height_km,
            0.1f,
            100.0f);
        validated.mie_scale_height_km = clamp_float(
            validated.mie_scale_height_km,
            0.1f,
            50.0f);
        validated.mie_g = clamp_float(validated.mie_g, -0.98f, 0.98f);
        validated.ozone_center_height_km = clamp_float(
            validated.ozone_center_height_km,
            5.0f,
            60.0f);
        validated.ozone_half_width_km = clamp_float(
            validated.ozone_half_width_km,
            1.0f,
            40.0f);
        validated.max_scattering_order =
            std::clamp<std::uint32_t>(validated.max_scattering_order, 2, 4);
        validated.solar_intensity = clamp_float(
            validated.solar_intensity,
            0.0f,
            1000.0f);

        const auto clamp_rgb = [](DirectX::XMFLOAT3& value,
            const float minimum,
            const float maximum)
        {
            value.x = clamp_float(value.x, minimum, maximum);
            value.y = clamp_float(value.y, minimum, maximum);
            value.z = clamp_float(value.z, minimum, maximum);
        };

        clamp_rgb(validated.rayleigh_scattering, 0.0f, 0.1f);
        clamp_rgb(validated.mie_scattering, 0.0f, 0.1f);
        clamp_rgb(validated.mie_extinction, 0.0f, 0.1f);
        clamp_rgb(validated.ozone_absorption, 0.0f, 0.1f);
        clamp_rgb(validated.ground_albedo, 0.0f, 1.0f);

        const std::uint64_t parameter_hash =
            calculate_parameter_hash(validated);
        _parameters = validated;

        if (_static_luts_valid && parameter_hash == _static_parameter_hash)
        {
            return true;
        }

        _static_luts_valid = false;

        atmosphere_constants constants{};
        set_float3(constants.rayleigh_scattering, validated.rayleigh_scattering);
        constants.planet_radius_km = validated.planet_radius_km;
        set_float3(constants.mie_scattering, validated.mie_scattering);
        constants.atmosphere_height_km = validated.atmosphere_height_km;
        set_float3(constants.mie_extinction, validated.mie_extinction);
        constants.rayleigh_scale_height_km = validated.rayleigh_scale_height_km;
        set_float3(constants.ozone_absorption, validated.ozone_absorption);
        constants.mie_scale_height_km = validated.mie_scale_height_km;
        set_float3(constants.ground_albedo, validated.ground_albedo);
        constants.mie_g = validated.mie_g;
        constants.solar_intensity = validated.solar_intensity;
        constants.ozone_center_height_km = validated.ozone_center_height_km;
        constants.ozone_half_width_km = validated.ozone_half_width_km;
        constants.sun_angular_radius = 0.004675f;
        constants.mu_s_min = -0.207912f;
        constants.max_scattering_order = validated.max_scattering_order;
        constants.angular_theta_samples = 4;
        constants.angular_phi_samples = 8;
        constants.reserved = 0;

        _context->UpdateSubresource(
            _atmosphere_cb.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        if (load_static_cache(parameter_hash))
        {
            _static_parameter_hash = parameter_hash;
            _static_luts_valid = true;
            return true;
        }

        _profiler.begin_scope(gpu_profile_scope::precompute);
        generate_static_luts();
        _profiler.end_scope(gpu_profile_scope::precompute);

        save_static_cache(parameter_hash);
        _static_parameter_hash = parameter_hash;
        _static_luts_valid = true;
        return true;
    }

    void atmosphere_renderer::set_exposure(const float exposure)
    {
        _parameters.exposure = clamp_float(exposure, 0.01f, 16.0f);
    }

    void atmosphere_renderer::update_current_sky(const solar_state& current)
    {
        if (_initialized &&
            _static_luts_valid &&
            _render_method == render_method::sky_current)
        {
            _profiler.begin_scope(gpu_profile_scope::sky_generation);
            generate_sky_view(_sky_current_uav.Get(), current);
            _profiler.end_scope(gpu_profile_scope::sky_generation);
        }
    }

    void atmosphere_renderer::update_sky_samples(
        const solar_state& sample_a,
        const solar_state& sample_b,
        const solar_state& current,
        const float blend_factor)
    {
        (void)blend_factor;

        if (!_initialized || !_static_luts_valid)
        {
            return;
        }

        const bool requires_sky_generation =
            _render_method == render_method::sky_current ||
            _render_method == render_method::sky_blend_pixel ||
            _render_method == render_method::sky_blend_compute;

        if (!requires_sky_generation)
        {
            return;
        }

        _profiler.begin_scope(gpu_profile_scope::sky_generation);

        if (_render_method == render_method::sky_current)
        {
            generate_sky_view(_sky_current_uav.Get(), current);
        }
        else
        {
            generate_sky_view(_sky_a_uav.Get(), sample_a);
            generate_sky_view(_sky_b_uav.Get(), sample_b);
        }

        _profiler.end_scope(gpu_profile_scope::sky_generation);
    }

    void atmosphere_renderer::draw(
        ID3D11RenderTargetView* target,
        const unsigned int width,
        const unsigned int height,
        const solar_state& current_sun,
        const float blend_factor)
    {
        if (!_initialized || !_static_luts_valid ||
            target == nullptr || width == 0 || height == 0)
        {
            return;
        }

        if (_render_method == render_method::sky_blend_compute)
        {
            _profiler.begin_scope(gpu_profile_scope::blend);
            blend_sky_views(blend_factor);
            _profiler.end_scope(gpu_profile_scope::blend);
        }

        update_constant_buffers(current_sun, blend_factor);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        _context->OMSetRenderTargets(1, &target, nullptr);
        _context->RSSetViewports(1, &viewport);
        _context->RSSetState(_rasterizer_state.Get());
        _context->IASetInputLayout(nullptr);
        _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _context->VSSetShader(_fullscreen_vs.Get(), nullptr, 0);

        ID3D11Buffer* constant_buffers[] =
        {
            _frame_cb.Get(),
            _atmosphere_cb.Get()
        };
        _context->PSSetConstantBuffers(0, 2, constant_buffers);

        ID3D11SamplerState* linear_sampler = _linear_sampler.Get();
        ID3D11SamplerState* clamp_sampler = _clamp_sampler.Get();
        _context->PSSetSamplers(0, 1, &linear_sampler);
        _context->PSSetSamplers(1, 1, &clamp_sampler);

        ID3D11ShaderResourceView* resources[11] =
        {
            _transmittance_srv.Get(),
            _single_rayleigh_srv.Get(),
            _single_mie_srv.Get(),
            _delta_multiple_srv.Get(),
            _irradiance_sum_a_srv.Get(),
            _scattering_density_srv.Get(),
            _sky_a_srv.Get(),
            _sky_b_srv.Get(),
            _sky_current_srv.Get(),
            _scattering_sum_a_srv.Get(),
            _single_mie_srv.Get()
        };
        _context->PSSetShaderResources(0, 11, resources);

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
        _profiler.begin_scope(gpu_profile_scope::render);
        _context->Draw(3, 0);
        _profiler.end_scope(gpu_profile_scope::render);

        ID3D11ShaderResourceView* null_resources[11]{};
        _context->PSSetShaderResources(0, 11, null_resources);
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

    void atmosphere_renderer::begin_profile_frame()
    {
        _profiler.begin_frame();
    }

    void atmosphere_renderer::end_profile_frame()
    {
        _profiler.end_frame();
    }

    bool atmosphere_renderer::get_profile_result(gpu_profile_result& result)
    {
        return _profiler.poll(result);
    }


    bool atmosphere_renderer::render_hdr_target(
        ID3D11RenderTargetView* target,
        const unsigned int width,
        const unsigned int height,
        const solar_state& sun,
        const float blend_factor,
        ID3D11PixelShader* pixel_shader)
    {
        if (!_initialized || target == nullptr || pixel_shader == nullptr ||
            width == 0 || height == 0)
        {
            return false;
        }

        update_constant_buffers(sun, blend_factor);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        _context->OMSetRenderTargets(1, &target, nullptr);
        _context->OMSetDepthStencilState(nullptr, 0);
        _context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
        _context->RSSetViewports(1, &viewport);
        _context->RSSetState(_rasterizer_state.Get());
        _context->IASetInputLayout(nullptr);
        _context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _context->VSSetShader(_fullscreen_vs.Get(), nullptr, 0);

        ID3D11Buffer* constant_buffers[] =
        {
            _frame_cb.Get(),
            _atmosphere_cb.Get()
        };
        _context->PSSetConstantBuffers(0, 2, constant_buffers);

        ID3D11SamplerState* linear_sampler = _linear_sampler.Get();
        ID3D11SamplerState* clamp_sampler = _clamp_sampler.Get();
        _context->PSSetSamplers(0, 1, &linear_sampler);
        _context->PSSetSamplers(1, 1, &clamp_sampler);

        ID3D11ShaderResourceView* resources[11] =
        {
            _transmittance_srv.Get(),
            _single_rayleigh_srv.Get(),
            _single_mie_srv.Get(),
            _delta_multiple_srv.Get(),
            _irradiance_sum_a_srv.Get(),
            _scattering_density_srv.Get(),
            _sky_a_srv.Get(),
            _sky_b_srv.Get(),
            _sky_current_srv.Get(),
            _scattering_sum_a_srv.Get(),
            _single_mie_srv.Get()
        };
        _context->PSSetShaderResources(0, 11, resources);
        _context->PSSetShader(pixel_shader, nullptr, 0);
        _context->Draw(3, 0);

        ID3D11ShaderResourceView* null_resources[11]{};
        _context->PSSetShaderResources(0, 11, null_resources);
        _context->OMSetRenderTargets(0, nullptr, nullptr);
        return true;
    }

    bool atmosphere_renderer::analyze_quality(
        const solar_state& sample_a,
        const solar_state& sample_b,
        const solar_state& current,
        const float blend_factor,
        const unsigned int output_width,
        const unsigned int output_height,
        sky_quality_metrics& blend_metrics,
        sky_quality_metrics& direct_metrics)
    {
        blend_metrics = {};
        direct_metrics = {};

        if (!_initialized || !_static_luts_valid ||
            output_width == 0 || output_height == 0)
        {
            return false;
        }

        generate_sky_view(_sky_a_uav.Get(), sample_a);
        generate_sky_view(_sky_b_uav.Get(), sample_b);
        generate_sky_view(_quality_reference_uav.Get(), current);
        blend_sky_views(blend_factor);

        std::vector<float> current_reference;
        std::vector<float> interpolated;
        unsigned int reference_width = 0;
        unsigned int reference_height = 0;
        unsigned int interpolated_width = 0;
        unsigned int interpolated_height = 0;

        if (!read_texture_rgb16f_pixels(
                _device.Get(),
                _context.Get(),
                _quality_reference_texture.Get(),
                current_reference,
                reference_width,
                reference_height) ||
            !read_texture_rgb16f_pixels(
                _device.Get(),
                _context.Get(),
                _sky_current_texture.Get(),
                interpolated,
                interpolated_width,
                interpolated_height) ||
            reference_width != interpolated_width ||
            reference_height != interpolated_height ||
            !calculate_quality_metrics(
                current_reference,
                interpolated,
                reference_width,
                reference_height,
                blend_metrics))
        {
            return false;
        }

        double scale = 1.0;
        scale = std::min(
            scale,
            1920.0 / static_cast<double>(output_width));
        scale = std::min(
            scale,
            1080.0 / static_cast<double>(output_height));

        const unsigned int analysis_width = std::max(
            1U,
            static_cast<unsigned int>(
                std::floor(static_cast<double>(output_width) * scale)));
        const unsigned int analysis_height = std::max(
            1U,
            static_cast<unsigned int>(
                std::floor(static_cast<double>(output_height) * scale)));

        const auto render_to_pixels =
            [&](ID3D11PixelShader* pixel_shader,
                std::vector<float>& pixels) -> bool
        {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = analysis_width;
            description.Height = analysis_height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = lut_format;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_RENDER_TARGET;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
            if (FAILED(_device->CreateTexture2D(
                    &description,
                    nullptr,
                    texture.GetAddressOf())) ||
                FAILED(_device->CreateRenderTargetView(
                    texture.Get(),
                    nullptr,
                    target.GetAddressOf())))
            {
                return false;
            }

            if (!render_hdr_target(
                target.Get(),
                analysis_width,
                analysis_height,
                current,
                blend_factor,
                pixel_shader))
            {
                return false;
            }

            unsigned int read_width = 0;
            unsigned int read_height = 0;
            return read_texture_rgb16f_pixels(
                _device.Get(),
                _context.Get(),
                texture.Get(),
                pixels,
                read_width,
                read_height) &&
                read_width == analysis_width &&
                read_height == analysis_height;
        };

        std::vector<float> direct_pixels;
        std::vector<float> blended_pixels;
        if (!render_to_pixels(_direct_radiance_ps.Get(), direct_pixels) ||
            !render_to_pixels(_final_radiance_ps.Get(), blended_pixels) ||
            !calculate_quality_metrics(
                direct_pixels,
                blended_pixels,
                analysis_width,
                analysis_height,
                direct_metrics))
        {
            return false;
        }

        if (_render_method == render_method::sky_current)
        {
            generate_sky_view(_sky_current_uav.Get(), current);
        }

        return true;
    }

    bool atmosphere_renderer::capture_sky_pfm(
        const std::filesystem::path& path,
        const solar_state& sample_a,
        const solar_state& sample_b,
        const solar_state& current,
        const float blend_factor)
    {
        if (!_initialized || !_static_luts_valid || path.empty())
        {
            return false;
        }

        if (_render_method == render_method::direct ||
            _render_method == render_method::sky_current)
        {
            generate_sky_view(_sky_current_uav.Get(), current);
        }
        else
        {
            generate_sky_view(_sky_a_uav.Get(), sample_a);
            generate_sky_view(_sky_b_uav.Get(), sample_b);
            blend_sky_views(blend_factor);
        }

        std::vector<float> pixels;
        unsigned int width = 0;
        unsigned int height = 0;
        if (!read_texture_rgb16f_pixels(
            _device.Get(),
            _context.Get(),
            _sky_current_texture.Get(),
            pixels,
            width,
            height))
        {
            return false;
        }

        std::error_code directory_error;
        if (!path.parent_path().empty())
        {
            std::filesystem::create_directories(
                path.parent_path(),
                directory_error);
            if (directory_error)
            {
                return false;
            }
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return false;
        }

        file << "PF\n" << width << " " << height << "\n-1.0\n";
        if (!file.good())
        {
            return false;
        }

        for (unsigned int row = height; row > 0; --row)
        {
            const unsigned int y = row - 1U;
            const std::size_t row_offset =
                static_cast<std::size_t>(y) * width * 3U;
            const std::size_t row_bytes =
                static_cast<std::size_t>(width) * 3U * sizeof(float);
            file.write(
                reinterpret_cast<const char*>(pixels.data() + row_offset),
                static_cast<std::streamsize>(row_bytes));
            if (!file.good())
            {
                return false;
            }
        }

        return true;
    }

    bool atmosphere_renderer::create_shaders(const wchar_t* shader_path)
    {
        const std::filesystem::path path =
            std::filesystem::path(shader_path) / L"atmosphere.hlsl";

        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;

        const auto create_compute = [&](const char* entry, ID3D11ComputeShader** shader) -> bool
        {
            if (!compile_shader(
                path.c_str(),
                entry,
                "cs_5_0",
                bytecode.ReleaseAndGetAddressOf()))
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
            if (!compile_shader(
                path.c_str(),
                entry,
                "ps_5_0",
                bytecode.ReleaseAndGetAddressOf()))
            {
                return false;
            }

            return SUCCEEDED(_device->CreatePixelShader(
                bytecode->GetBufferPointer(),
                bytecode->GetBufferSize(),
                nullptr,
                shader));
        };

        const char* compute_entries[] =
        {
            "CS_Transmittance",
            "CS_DirectIrradiance",
            "CS_SingleScattering",
            "CS_ScatteringDensity",
            "CS_MultipleScattering",
            "CS_IndirectIrradiance",
            "CS_AccumulateScattering",
            "CS_AccumulateIrradiance",
            "CS_SkyView",
            "CS_Blend"
        };

        ID3D11ComputeShader** compute_shaders[] =
        {
            _transmittance_cs.GetAddressOf(),
            _direct_irradiance_cs.GetAddressOf(),
            _single_scattering_cs.GetAddressOf(),
            _scattering_density_cs.GetAddressOf(),
            _multiple_scattering_cs.GetAddressOf(),
            _indirect_irradiance_cs.GetAddressOf(),
            _accumulate_scattering_cs.GetAddressOf(),
            _accumulate_irradiance_cs.GetAddressOf(),
            _sky_view_cs.GetAddressOf(),
            _blend_cs.GetAddressOf()
        };

        for (std::size_t index = 0; index < std::size(compute_entries); ++index)
        {
            if (!create_compute(compute_entries[index], compute_shaders[index]))
            {
                return false;
            }
        }

        if (!compile_shader(
            path.c_str(),
            "VS_Fullscreen",
            "vs_5_0",
            bytecode.ReleaseAndGetAddressOf()) ||
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
            !create_pixel("PS_SkyFinal", _final_ps.GetAddressOf()) ||
            !create_pixel("PS_DirectRadiance", _direct_radiance_ps.GetAddressOf()) ||
            !create_pixel("PS_SkyFinalRadiance", _final_radiance_ps.GetAddressOf()))
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

        if (FAILED(_device->CreateSamplerState(
            &sampler_description,
            _linear_sampler.GetAddressOf())))
        {
            return false;
        }

        D3D11_RASTERIZER_DESC rasterizer_description{};
        rasterizer_description.FillMode = D3D11_FILL_SOLID;
        rasterizer_description.CullMode = D3D11_CULL_NONE;
        rasterizer_description.DepthClipEnable = TRUE;

        if (FAILED(_device->CreateRasterizerState(
            &rasterizer_description,
            _rasterizer_state.GetAddressOf())))
        {
            return false;
        }

        sampler_description.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

        return SUCCEEDED(_device->CreateSamplerState(
            &sampler_description,
            _clamp_sampler.GetAddressOf()));
    }

    bool atmosphere_renderer::create_constant_buffers()
    {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = sizeof(frame_constants);
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

        if (FAILED(_device->CreateBuffer(
            &description,
            nullptr,
            _frame_cb.GetAddressOf())))
        {
            return false;
        }

        description.ByteWidth = sizeof(atmosphere_constants);
        return SUCCEEDED(_device->CreateBuffer(
            &description,
            nullptr,
            _atmosphere_cb.GetAddressOf()));
    }

    bool atmosphere_renderer::create_textures()
    {
        return create_render_target(
                transmittance_width,
                transmittance_height,
                _transmittance_texture.GetAddressOf(),
                _transmittance_srv.GetAddressOf(),
                _transmittance_uav.GetAddressOf()) &&
            create_static_scattering_textures() &&
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
                _sky_current_uav.GetAddressOf()) &&
            create_render_target(
                sky_width,
                sky_height,
                _quality_reference_texture.GetAddressOf(),
                _quality_reference_srv.GetAddressOf(),
                _quality_reference_uav.GetAddressOf());
    }

    bool atmosphere_renderer::create_static_scattering_textures()
    {
        const bool volumes_created =
            create_volume_texture(
                _single_rayleigh_texture.GetAddressOf(),
                _single_rayleigh_srv.GetAddressOf(),
                _single_rayleigh_uav.GetAddressOf()) &&
            create_volume_texture(
                _single_mie_texture.GetAddressOf(),
                _single_mie_srv.GetAddressOf(),
                _single_mie_uav.GetAddressOf()) &&
            create_volume_texture(
                _scattering_sum_a_texture.GetAddressOf(),
                _scattering_sum_a_srv.GetAddressOf(),
                _scattering_sum_a_uav.GetAddressOf()) &&
            create_volume_texture(
                _scattering_sum_b_texture.GetAddressOf(),
                _scattering_sum_b_srv.GetAddressOf(),
                _scattering_sum_b_uav.GetAddressOf()) &&
            create_volume_texture(
                _scattering_density_texture.GetAddressOf(),
                _scattering_density_srv.GetAddressOf(),
                _scattering_density_uav.GetAddressOf()) &&
            create_volume_texture(
                _delta_multiple_texture.GetAddressOf(),
                _delta_multiple_srv.GetAddressOf(),
                _delta_multiple_uav.GetAddressOf());

        if (!volumes_created)
        {
            return false;
        }

        const bool irradiance_created =
            create_render_target(
                irradiance_width,
                irradiance_height,
                _direct_irradiance_texture.GetAddressOf(),
                _direct_irradiance_srv.GetAddressOf(),
                _direct_irradiance_uav.GetAddressOf()) &&
            create_render_target(
                irradiance_width,
                irradiance_height,
                _delta_irradiance_texture.GetAddressOf(),
                _delta_irradiance_srv.GetAddressOf(),
                _delta_irradiance_uav.GetAddressOf()) &&
            create_render_target(
                irradiance_width,
                irradiance_height,
                _next_irradiance_texture.GetAddressOf(),
                _next_irradiance_srv.GetAddressOf(),
                _next_irradiance_uav.GetAddressOf()) &&
            create_render_target(
                irradiance_width,
                irradiance_height,
                _irradiance_sum_a_texture.GetAddressOf(),
                _irradiance_sum_a_srv.GetAddressOf(),
                _irradiance_sum_a_uav.GetAddressOf()) &&
            create_render_target(
                irradiance_width,
                irradiance_height,
                _irradiance_sum_b_texture.GetAddressOf(),
                _irradiance_sum_b_srv.GetAddressOf(),
                _irradiance_sum_b_uav.GetAddressOf());

        return irradiance_created;
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
        description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE |
            D3D11_BIND_UNORDERED_ACCESS;

        if (FAILED(_device->CreateTexture2D(
            &description,
            nullptr,
            texture)))
        {
            return false;
        }

        if (FAILED(_device->CreateShaderResourceView(
            *texture,
            nullptr,
            srv)))
        {
            return false;
        }

        return SUCCEEDED(_device->CreateUnorderedAccessView(
            *texture,
            nullptr,
            uav));
    }

    bool atmosphere_renderer::create_volume_texture(
        ID3D11Texture3D** texture,
        ID3D11ShaderResourceView** srv,
        ID3D11UnorderedAccessView** uav)
    {
        D3D11_TEXTURE3D_DESC description{};
        description.Width = scattering_width;
        description.Height = scattering_height;
        description.Depth = scattering_depth;
        description.MipLevels = 1;
        description.Format = lut_format;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE |
            D3D11_BIND_UNORDERED_ACCESS;

        if (FAILED(_device->CreateTexture3D(
            &description,
            nullptr,
            texture)))
        {
            return false;
        }

        if (FAILED(_device->CreateShaderResourceView(
            *texture,
            nullptr,
            srv)))
        {
            return false;
        }

        return SUCCEEDED(_device->CreateUnorderedAccessView(
            *texture,
            nullptr,
            uav));
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
        const float blend_factor,
        const std::uint32_t scattering_order)
    {
        frame_constants constants{};
        constants.sun_direction[0] = sun.direction.x;
        constants.sun_direction[1] = sun.direction.y;
        constants.sun_direction[2] = sun.direction.z;
        constants.exposure = _parameters.exposure;
        constants.blend_factor = blend_factor;
        constants.scattering_order = scattering_order;

        _context->UpdateSubresource(
            _frame_cb.Get(),
            0,
            nullptr,
            &constants,
            0,
            0);

        ID3D11Buffer* buffers[] =
        {
            _frame_cb.Get(),
            _atmosphere_cb.Get()
        };
        _context->CSSetConstantBuffers(0, 2, buffers);
    }

    bool atmosphere_renderer::load_static_cache(
        const std::uint64_t parameter_hash)
    {
        const std::filesystem::path cache_path =
            get_static_cache_path(parameter_hash);
        if (cache_path.empty())
        {
            return false;
        }

        std::ifstream file(cache_path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static_cache_header header{};
        file.read(
            reinterpret_cast<char*>(&header),
            static_cast<std::streamsize>(sizeof(header)));

        const bool header_valid =
            file.good() &&
            std::memcmp(header.magic, "DWLUT001", 8) == 0 &&
            header.version == 1 &&
            header.format == static_cast<std::uint32_t>(lut_format) &&
            header.parameter_hash == parameter_hash &&
            header.transmittance_width == transmittance_width &&
            header.transmittance_height == transmittance_height &&
            header.irradiance_width == irradiance_width &&
            header.irradiance_height == irradiance_height &&
            header.scattering_width == scattering_width &&
            header.scattering_height == scattering_height &&
            header.scattering_depth == scattering_depth;

        if (!header_valid)
        {
            file.close();
            DeleteFileW(cache_path.c_str());
            return false;
        }

        bool success =
            read_texture_2d(
                _device.Get(),
                _context.Get(),
                _transmittance_texture.Get(),
                file) &&
            read_texture_2d(
                _device.Get(),
                _context.Get(),
                _direct_irradiance_texture.Get(),
                file) &&
            read_texture_2d(
                _device.Get(),
                _context.Get(),
                _irradiance_sum_a_texture.Get(),
                file) &&
            read_texture_3d(
                _device.Get(),
                _context.Get(),
                _single_rayleigh_texture.Get(),
                file) &&
            read_texture_3d(
                _device.Get(),
                _context.Get(),
                _single_mie_texture.Get(),
                file) &&
            read_texture_3d(
                _device.Get(),
                _context.Get(),
                _scattering_sum_a_texture.Get(),
                file);

        if (success)
        {
            success = file.peek() == std::char_traits<char>::eof();
        }

        file.close();

        if (!success)
        {
            DeleteFileW(cache_path.c_str());
            return false;
        }

        return true;
    }

    bool atmosphere_renderer::save_static_cache(
        const std::uint64_t parameter_hash)
    {
        const std::filesystem::path cache_path =
            get_static_cache_path(parameter_hash);
        if (cache_path.empty())
        {
            return false;
        }

        std::filesystem::path temporary_path = cache_path;
        temporary_path += L".tmp";

        std::ofstream file(
            temporary_path,
            std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return false;
        }

        static_cache_header header{};
        std::memcpy(header.magic, "DWLUT001", 8);
        header.version = 1;
        header.format = static_cast<std::uint32_t>(lut_format);
        header.transmittance_width = transmittance_width;
        header.transmittance_height = transmittance_height;
        header.irradiance_width = irradiance_width;
        header.irradiance_height = irradiance_height;
        header.scattering_width = scattering_width;
        header.scattering_height = scattering_height;
        header.scattering_depth = scattering_depth;
        header.parameter_hash = parameter_hash;

        file.write(
            reinterpret_cast<const char*>(&header),
            static_cast<std::streamsize>(sizeof(header)));

        bool success = file.good() &&
            write_texture_2d(
                _device.Get(),
                _context.Get(),
                _transmittance_texture.Get(),
                file) &&
            write_texture_2d(
                _device.Get(),
                _context.Get(),
                _direct_irradiance_texture.Get(),
                file) &&
            write_texture_2d(
                _device.Get(),
                _context.Get(),
                _irradiance_sum_a_texture.Get(),
                file) &&
            write_texture_3d(
                _device.Get(),
                _context.Get(),
                _single_rayleigh_texture.Get(),
                file) &&
            write_texture_3d(
                _device.Get(),
                _context.Get(),
                _single_mie_texture.Get(),
                file) &&
            write_texture_3d(
                _device.Get(),
                _context.Get(),
                _scattering_sum_a_texture.Get(),
                file);

        file.flush();
        success = success && file.good();
        file.close();
        success = success && !file.fail();

        if (!success)
        {
            DeleteFileW(temporary_path.c_str());
            return false;
        }

        if (!MoveFileExW(
            temporary_path.c_str(),
            cache_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temporary_path.c_str());
            return false;
        }

        return true;
    }

    void atmosphere_renderer::generate_static_luts()
    {
        generate_transmittance();
        generate_direct_irradiance();
        generate_single_scattering();
        generate_multiple_scattering();
    }

    void atmosphere_renderer::generate_transmittance()
    {
        _profiler.begin_scope(gpu_profile_scope::transmittance);
        update_constant_buffers(solar_state{}, 0.0f);

        ID3D11UnorderedAccessView* output = _transmittance_uav.Get();
        _context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);

        ID3D11ComputeShader* shader = _transmittance_cs.Get();
        _context->CSSetShader(shader, nullptr, 0);
        _context->Dispatch(
            (transmittance_width + 7) / 8,
            (transmittance_height + 7) / 8,
            1);
        clear_compute_bindings();
        _profiler.end_scope(gpu_profile_scope::transmittance);
    }

    void atmosphere_renderer::generate_direct_irradiance()
    {
        _profiler.begin_scope(gpu_profile_scope::direct_irradiance);
        update_constant_buffers(solar_state{}, 0.0f);

        ID3D11ShaderResourceView* transmittance = _transmittance_srv.Get();
        _context->CSSetShaderResources(0, 1, &transmittance);

        ID3D11SamplerState* sampler = _clamp_sampler.Get();
        _context->CSSetSamplers(1, 1, &sampler);

        ID3D11UnorderedAccessView* output = _direct_irradiance_uav.Get();
        _context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);

        _context->CSSetShader(_direct_irradiance_cs.Get(), nullptr, 0);
        _context->Dispatch(
            (irradiance_width + 7) / 8,
            (irradiance_height + 7) / 8,
            1);

        clear_compute_bindings();
        _profiler.end_scope(gpu_profile_scope::direct_irradiance);
    }

    void atmosphere_renderer::generate_single_scattering()
    {
        _profiler.begin_scope(gpu_profile_scope::single_scattering);
        update_constant_buffers(solar_state{}, 0.0f);

        ID3D11ShaderResourceView* transmittance = _transmittance_srv.Get();
        _context->CSSetShaderResources(0, 1, &transmittance);

        ID3D11SamplerState* sampler = _clamp_sampler.Get();
        _context->CSSetSamplers(1, 1, &sampler);

        ID3D11UnorderedAccessView* outputs[] =
        {
            _single_rayleigh_uav.Get(),
            _single_mie_uav.Get()
        };
        _context->CSSetUnorderedAccessViews(1, 2, outputs, nullptr);

        _context->CSSetShader(_single_scattering_cs.Get(), nullptr, 0);
        dispatch_3d(_single_scattering_cs.Get());
        clear_compute_bindings();

        _context->CopyResource(
            _scattering_sum_a_texture.Get(),
            _single_rayleigh_texture.Get());
        _profiler.end_scope(gpu_profile_scope::single_scattering);
    }

    void atmosphere_renderer::generate_multiple_scattering()
    {
        _profiler.begin_scope(gpu_profile_scope::multiple_scattering);
        const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        _context->ClearUnorderedAccessViewFloat(
            _irradiance_sum_a_uav.Get(),
            clear_color);
        _context->ClearUnorderedAccessViewFloat(
            _irradiance_sum_b_uav.Get(),
            clear_color);
        _context->ClearUnorderedAccessViewFloat(
            _delta_multiple_uav.Get(),
            clear_color);
        _context->CopyResource(
            _delta_irradiance_texture.Get(),
            _direct_irradiance_texture.Get());

        for (std::uint32_t scattering_order = 2;
            scattering_order <= _parameters.max_scattering_order;
            ++scattering_order)
        {
            update_constant_buffers(
                solar_state{},
                0.0f,
                scattering_order);

            ID3D11ShaderResourceView* density_inputs[] =
            {
                _transmittance_srv.Get(),
                _single_rayleigh_srv.Get(),
                _single_mie_srv.Get(),
                _delta_multiple_srv.Get(),
                _delta_irradiance_srv.Get()
            };
            _context->CSSetShaderResources(0, 5, density_inputs);

            ID3D11SamplerState* sampler = _clamp_sampler.Get();
            _context->CSSetSamplers(1, 1, &sampler);

            ID3D11UnorderedAccessView* density_output =
                _scattering_density_uav.Get();
            _context->CSSetUnorderedAccessViews(
                1,
                1,
                &density_output,
                nullptr);
            _context->CSSetShader(_scattering_density_cs.Get(), nullptr, 0);
            dispatch_3d(_scattering_density_cs.Get());
            clear_compute_bindings();

            update_constant_buffers(
                solar_state{},
                0.0f,
                scattering_order - 1);

            ID3D11ShaderResourceView* irradiance_inputs[] =
            {
                _transmittance_srv.Get(),
                _single_rayleigh_srv.Get(),
                _single_mie_srv.Get(),
                _delta_multiple_srv.Get()
            };
            _context->CSSetShaderResources(0, 4, irradiance_inputs);
            _context->CSSetSamplers(1, 1, &sampler);

            ID3D11UnorderedAccessView* next_irradiance_output =
                _next_irradiance_uav.Get();
            _context->CSSetUnorderedAccessViews(
                0,
                1,
                &next_irradiance_output,
                nullptr);
            _context->CSSetShader(_indirect_irradiance_cs.Get(), nullptr, 0);
            _context->Dispatch(
                (irradiance_width + 7) / 8,
                (irradiance_height + 7) / 8,
                1);
            clear_compute_bindings();

            ID3D11ShaderResourceView* irradiance_accumulation_inputs[] =
            {
                _irradiance_sum_a_srv.Get(),
                _next_irradiance_srv.Get()
            };
            _context->CSSetShaderResources(4, 1, &irradiance_accumulation_inputs[0]);
            _context->CSSetShaderResources(8, 1, &irradiance_accumulation_inputs[1]);

            ID3D11UnorderedAccessView* irradiance_accumulation_output =
                _irradiance_sum_b_uav.Get();
            _context->CSSetUnorderedAccessViews(
                0,
                1,
                &irradiance_accumulation_output,
                nullptr);
            _context->CSSetShader(_accumulate_irradiance_cs.Get(), nullptr, 0);
            _context->Dispatch(
                (irradiance_width + 7) / 8,
                (irradiance_height + 7) / 8,
                1);
            clear_compute_bindings();

            update_constant_buffers(
                solar_state{},
                0.0f,
                scattering_order);

            ID3D11ShaderResourceView* transmittance_input =
                _transmittance_srv.Get();
            ID3D11ShaderResourceView* density_input =
                _scattering_density_srv.Get();
            _context->CSSetShaderResources(0, 1, &transmittance_input);
            _context->CSSetShaderResources(5, 1, &density_input);
            _context->CSSetSamplers(1, 1, &sampler);

            ID3D11UnorderedAccessView* multiple_output =
                _delta_multiple_uav.Get();
            _context->CSSetUnorderedAccessViews(
                1,
                1,
                &multiple_output,
                nullptr);
            _context->CSSetShader(_multiple_scattering_cs.Get(), nullptr, 0);
            dispatch_3d(_multiple_scattering_cs.Get());
            clear_compute_bindings();

            ID3D11ShaderResourceView* scattering_accumulation_inputs[] =
            {
                _scattering_sum_a_srv.Get(),
                _single_rayleigh_srv.Get(),
                _single_mie_srv.Get(),
                _delta_multiple_srv.Get()
            };
            _context->CSSetShaderResources(
                3,
                1,
                &scattering_accumulation_inputs[3]);
            _context->CSSetShaderResources(
                9,
                1,
                &scattering_accumulation_inputs[0]);

            ID3D11UnorderedAccessView* scattering_accumulation_output =
                _scattering_sum_b_uav.Get();
            _context->CSSetUnorderedAccessViews(
                1,
                1,
                &scattering_accumulation_output,
                nullptr);
            _context->CSSetShader(_accumulate_scattering_cs.Get(), nullptr, 0);
            dispatch_3d(_accumulate_scattering_cs.Get());
            clear_compute_bindings();

            swap_com_ptr(_scattering_sum_a_texture, _scattering_sum_b_texture);
            swap_com_ptr(_scattering_sum_a_srv, _scattering_sum_b_srv);
            swap_com_ptr(_scattering_sum_a_uav, _scattering_sum_b_uav);

            swap_com_ptr(_irradiance_sum_a_texture, _irradiance_sum_b_texture);
            swap_com_ptr(_irradiance_sum_a_srv, _irradiance_sum_b_srv);
            swap_com_ptr(_irradiance_sum_a_uav, _irradiance_sum_b_uav);

            swap_com_ptr(_delta_irradiance_texture, _next_irradiance_texture);
            swap_com_ptr(_delta_irradiance_srv, _next_irradiance_srv);
            swap_com_ptr(_delta_irradiance_uav, _next_irradiance_uav);
        }

        _profiler.end_scope(gpu_profile_scope::multiple_scattering);
    }

    void atmosphere_renderer::generate_sky_view(
        ID3D11UnorderedAccessView* target,
        const solar_state& sun)
    {
        if (target == nullptr || !_static_luts_valid)
        {
            return;
        }

        update_constant_buffers(sun, 0.0f);

        ID3D11ShaderResourceView* static_resources[] =
        {
            _scattering_sum_a_srv.Get(),
            _single_mie_srv.Get()
        };
        _context->CSSetShaderResources(9, 2, static_resources);

        ID3D11SamplerState* sampler = _clamp_sampler.Get();
        _context->CSSetSamplers(1, 1, &sampler);

        _context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        _context->CSSetShader(_sky_view_cs.Get(), nullptr, 0);
        _context->Dispatch(
            (sky_width + 7) / 8,
            (sky_height + 7) / 8,
            1);
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
        _context->CSSetShaderResources(6, 2, inputs);

        ID3D11UnorderedAccessView* output = _sky_current_uav.Get();
        _context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        _context->CSSetShader(_blend_cs.Get(), nullptr, 0);
        _context->Dispatch(
            (sky_width + 7) / 8,
            (sky_height + 7) / 8,
            1);
        clear_compute_bindings();
    }

    void atmosphere_renderer::dispatch_3d(ID3D11ComputeShader* shader)
    {
        _context->CSSetShader(shader, nullptr, 0);
        _context->Dispatch(
            (scattering_width + 3) / 4,
            (scattering_height + 3) / 4,
            (scattering_depth + 3) / 4);
    }

    void atmosphere_renderer::clear_compute_bindings()
    {
        ID3D11ShaderResourceView* null_srvs[11]{};
        ID3D11UnorderedAccessView* null_uavs[4]{};

        _context->CSSetShaderResources(0, 11, null_srvs);
        _context->CSSetUnorderedAccessViews(0, 4, null_uavs, nullptr);
        _context->CSSetShader(nullptr, nullptr, 0);
    }
}
