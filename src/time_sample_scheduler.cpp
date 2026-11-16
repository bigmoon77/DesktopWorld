#include "desktopworld/time_sample_scheduler.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace desktopworld
{
    std::int64_t floor_divide(
        const std::int64_t value,
        const std::int64_t divisor)
    {
        if (divisor <= 0)
        {
            throw std::invalid_argument("divisor must be positive");
        }

        std::int64_t quotient = value / divisor;
        const std::int64_t remainder = value % divisor;

        if (remainder < 0)
        {
            --quotient;
        }

        return quotient;
    }

    std::int64_t calculate_sample_interval_seconds(
        const float solar_elevation_degrees,
        const int interval_override_seconds)
    {
        if (interval_override_seconds == 900 ||
            interval_override_seconds == 1800 ||
            interval_override_seconds == 3600)
        {
            return interval_override_seconds;
        }

        if (std::isfinite(solar_elevation_degrees) &&
            std::abs(solar_elevation_degrees) <= 6.0f)
        {
            return 900;
        }

        if (std::isfinite(solar_elevation_degrees) &&
            std::abs(solar_elevation_degrees) <= 12.0f)
        {
            return 1800;
        }

        return 3600;
    }

    time_sample_window calculate_time_sample_window(
        const std::int64_t current_utc_seconds,
        const std::int64_t interval_seconds)
    {
        if (interval_seconds <= 0)
        {
            throw std::invalid_argument("interval_seconds must be positive");
        }

        time_sample_window result{};
        result.interval_seconds = interval_seconds;
        result.start_utc_seconds =
            floor_divide(current_utc_seconds, interval_seconds) *
            interval_seconds;
        result.end_utc_seconds =
            result.start_utc_seconds + interval_seconds;

        const double numerator = static_cast<double>(
            current_utc_seconds - result.start_utc_seconds);
        const double denominator = static_cast<double>(interval_seconds);

        result.blend_factor = static_cast<float>(std::clamp(
            numerator / denominator,
            0.0,
            1.0));

        return result;
    }
}
