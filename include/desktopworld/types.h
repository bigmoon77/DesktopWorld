#pragma once

#include <DirectXMath.h>
#include <cstdint>

namespace desktopworld
{
    enum class render_method : std::uint32_t
    {
        direct = 0,
        sky_current = 1,
        sky_blend_pixel = 2,
        sky_blend_compute = 3
    };

    enum class frame_mode : std::uint32_t
    {
        adaptive = 0,
        fixed = 1,
        on_demand = 2
    };

    struct solar_state
    {
        DirectX::XMFLOAT3 direction{ 0.0f, 1.0f, 0.0f };
        float elevation_degrees = 90.0f;
        float azimuth_degrees = 0.0f;
    };

    struct atmosphere_parameters
    {
        float planet_radius_km = 6360.0f;
        float atmosphere_height_km = 100.0f;
        DirectX::XMFLOAT3 rayleigh_scattering{ 0.005802f, 0.013558f, 0.033100f };
        float rayleigh_scale_height_km = 8.0f;
        DirectX::XMFLOAT3 mie_scattering{ 0.003996f, 0.003996f, 0.003996f };
        float mie_scale_height_km = 1.2f;
        float mie_g = 0.8f;
        float solar_intensity = 20.0f;
        float exposure = 1.0f;
    };

    struct performance_stats
    {
        float cpu_frame_ms = 0.0f;
        float cpu_update_ms = 0.0f;
        float gpu_frame_ms = 0.0f;
        float gpu_precompute_ms = 0.0f;
        float gpu_render_ms = 0.0f;
        float gpu_sky_generation_ms = 0.0f;
        float gpu_blend_ms = 0.0f;
        float current_fps = 0.0f;
    };

    inline float clamp_float(const float value, const float minimum, const float maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }
}
