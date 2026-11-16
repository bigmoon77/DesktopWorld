#pragma once

#include "desktopworld/time_sample_scheduler.h"
#include "desktopworld/solar_model.h"

#include <Windows.h>

namespace desktopworld
{
    struct environment_frame
    {
        solar_state current_sun{};
        solar_state sample_sun_a{};
        solar_state sample_sun_b{};
        time_sample_window sample_window{};
        bool sample_pair_changed = false;
    };

    class environment_controller
    {
    public:
        void set_observer_location(double latitude_degrees, double longitude_degrees);
        void set_sample_interval_override(int interval_seconds);
        void invalidate();

        environment_frame update(const SYSTEMTIME& utc_time);

        double latitude_degrees() const;
        double longitude_degrees() const;
        int sample_interval_override() const;

    private:
        static std::int64_t to_utc_epoch_seconds(const SYSTEMTIME& utc_time);
        static SYSTEMTIME from_utc_epoch_seconds(std::int64_t utc_epoch_seconds);

        double _latitude_degrees = 35.6812;
        double _longitude_degrees = 139.7671;
        int _sample_interval_override_seconds = 0;

        std::int64_t _cached_sample_start_seconds = 0;
        std::int64_t _cached_sample_interval_seconds = 0;

        solar_state _sample_sun_a{};
        solar_state _sample_sun_b{};

        bool _has_sample_pair = false;
        bool _dirty = true;
    };
}
