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
        DirectX::XMFLOAT3 mie_extinction{ 0.004440f, 0.004440f, 0.004440f };
        float mie_scale_height_km = 1.2f;
        float mie_g = 0.8f;
        DirectX::XMFLOAT3 ozone_absorption{ 0.000650f, 0.001881f, 0.000085f };
        float ozone_center_height_km = 25.0f;
        float ozone_half_width_km = 15.0f;
        DirectX::XMFLOAT3 ground_albedo{ 0.1f, 0.1f, 0.1f };
        float solar_intensity = 20.0f;
        std::uint32_t max_scattering_order = 4;
        float exposure = 1.0f;
    };

    struct performance_stats
    {
        float cpu_frame_ms = 0.0f;
        float cpu_update_ms = 0.0f;
        float gpu_frame_ms = 0.0f;
        float gpu_precompute_ms = 0.0f;
        float gpu_transmittance_ms = 0.0f;
        float gpu_direct_irradiance_ms = 0.0f;
        float gpu_single_scattering_ms = 0.0f;
        float gpu_multiple_scattering_ms = 0.0f;
        float gpu_render_ms = 0.0f;
        float gpu_sky_generation_ms = 0.0f;
        float gpu_blend_ms = 0.0f;
        float current_fps = 0.0f;
        float gpu_local_memory_usage_mb = 0.0f;
        float gpu_local_memory_budget_mb = 0.0f;
        float gpu_non_local_memory_usage_mb = 0.0f;
        float gpu_non_local_memory_budget_mb = 0.0f;
        bool gpu_local_memory_valid = false;
        bool gpu_non_local_memory_valid = false;
    };

    inline float clamp_float(const float value, const float minimum, const float maximum)
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }
}
