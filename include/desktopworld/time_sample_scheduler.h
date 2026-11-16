#pragma once

#include <cstdint>

namespace desktopworld
{
    struct time_sample_window
    {
        std::int64_t start_utc_seconds = 0;
        std::int64_t end_utc_seconds = 3600;
        std::int64_t interval_seconds = 3600;
        float blend_factor = 0.0f;
    };

    std::int64_t floor_divide(std::int64_t value, std::int64_t divisor);

    std::int64_t calculate_sample_interval_seconds(
        float solar_elevation_degrees,
        int interval_override_seconds);

    time_sample_window calculate_time_sample_window(
        std::int64_t current_utc_seconds,
        std::int64_t interval_seconds);
}
