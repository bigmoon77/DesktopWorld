#include "desktopworld/environment_controller.h"

#include <algorithm>
#include <cstdint>

namespace desktopworld
{
    namespace
    {
        constexpr std::int64_t windows_to_unix_seconds = 11644473600LL;
        constexpr std::int64_t maximum_unix_seconds = 253402300799LL;
        constexpr std::uint64_t ticks_per_second = 10000000ULL;
    }

    void environment_controller::set_observer_location(
        const double latitude_degrees,
        const double longitude_degrees)
    {
        const double validated_latitude = std::clamp(latitude_degrees, -90.0, 90.0);
        const double validated_longitude = std::clamp(longitude_degrees, -180.0, 180.0);

        if (validated_latitude != _latitude_degrees ||
            validated_longitude != _longitude_degrees)
        {
            _latitude_degrees = validated_latitude;
            _longitude_degrees = validated_longitude;
            _dirty = true;
        }
    }

    void environment_controller::set_sample_interval_override(const int interval_seconds)
    {
        const int validated_interval =
            interval_seconds == 900 ||
            interval_seconds == 1800 ||
            interval_seconds == 3600
            ? interval_seconds
            : 0;

        if (validated_interval != _sample_interval_override_seconds)
        {
            _sample_interval_override_seconds = validated_interval;
            _dirty = true;
        }
    }

    void environment_controller::invalidate()
    {
        _dirty = true;
    }

    environment_frame environment_controller::update(const SYSTEMTIME& utc_time)
    {
        environment_frame result{};

        const std::int64_t current_epoch_seconds = to_utc_epoch_seconds(utc_time);
        result.current_sun = solar_model::calculate(
            utc_time,
            _latitude_degrees,
            _longitude_degrees);

        const std::int64_t interval_seconds = calculate_sample_interval_seconds(
            result.current_sun.elevation_degrees,
            _sample_interval_override_seconds);

        result.sample_window = calculate_time_sample_window(
            current_epoch_seconds,
            interval_seconds);

        const bool sample_pair_changed =
            !_has_sample_pair ||
            _cached_sample_start_seconds != result.sample_window.start_utc_seconds ||
            _cached_sample_interval_seconds != result.sample_window.interval_seconds;

        if (sample_pair_changed || _dirty)
        {
            const SYSTEMTIME sample_time_a = from_utc_epoch_seconds(
                result.sample_window.start_utc_seconds);
            const SYSTEMTIME sample_time_b = from_utc_epoch_seconds(
                result.sample_window.end_utc_seconds);

            _sample_sun_a = solar_model::calculate(
                sample_time_a,
                _latitude_degrees,
                _longitude_degrees);
            _sample_sun_b = solar_model::calculate(
                sample_time_b,
                _latitude_degrees,
                _longitude_degrees);

            _cached_sample_start_seconds = result.sample_window.start_utc_seconds;
            _cached_sample_interval_seconds = result.sample_window.interval_seconds;
            _has_sample_pair = true;
            _dirty = false;
            result.sample_pair_changed = true;
        }

        result.sample_sun_a = _sample_sun_a;
        result.sample_sun_b = _sample_sun_b;
        return result;
    }

    double environment_controller::latitude_degrees() const
    {
        return _latitude_degrees;
    }

    double environment_controller::longitude_degrees() const
    {
        return _longitude_degrees;
    }

    int environment_controller::sample_interval_override() const
    {
        return _sample_interval_override_seconds;
    }

    std::int64_t environment_controller::to_utc_epoch_seconds(
        const SYSTEMTIME& utc_time)
    {
        FILETIME file_time{};
        if (!SystemTimeToFileTime(&utc_time, &file_time))
        {
            return 0;
        }

        ULARGE_INTEGER value{};
        value.LowPart = file_time.dwLowDateTime;
        value.HighPart = file_time.dwHighDateTime;

        const std::int64_t file_time_seconds =
            static_cast<std::int64_t>(value.QuadPart / ticks_per_second);
        return file_time_seconds - windows_to_unix_seconds;
    }

    SYSTEMTIME environment_controller::from_utc_epoch_seconds(
        const std::int64_t utc_epoch_seconds)
    {
        const std::int64_t clamped_seconds = std::clamp(
            utc_epoch_seconds,
            -windows_to_unix_seconds,
            maximum_unix_seconds);

        const std::uint64_t file_time_ticks =
            static_cast<std::uint64_t>(
                clamped_seconds + windows_to_unix_seconds) *
            ticks_per_second;

        ULARGE_INTEGER value{};
        value.QuadPart = file_time_ticks;

        FILETIME file_time{};
        file_time.dwLowDateTime = value.LowPart;
        file_time.dwHighDateTime = value.HighPart;

        SYSTEMTIME result{};
        if (!FileTimeToSystemTime(&file_time, &result))
        {
            return SYSTEMTIME{};
        }

        return result;
    }
}
