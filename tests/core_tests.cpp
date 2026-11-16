#include "desktopworld/frame_controller.h"
#include "desktopworld/solar_model.h"
#include "desktopworld/time_sample_scheduler.h"

#include <Windows.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
    bool nearly_equal(const double a, const double b, const double tolerance)
    {
        return std::abs(a - b) <= tolerance;
    }

    SYSTEMTIME make_utc_time(
        const WORD year,
        const WORD month,
        const WORD day,
        const WORD hour,
        const WORD minute)
    {
        SYSTEMTIME result{};
        result.wYear = year;
        result.wMonth = month;
        result.wDay = day;
        result.wHour = hour;
        result.wMinute = minute;
        return result;
    }

    void test_solar_position()
    {
        const double latitude = 35.6812;
        const double longitude = 139.7671;

        const desktopworld::solar_state noon = desktopworld::solar_model::calculate(
            make_utc_time(2024, 3, 20, 3, 0),
            latitude,
            longitude);

        assert(noon.elevation_degrees > 50.0f);
        assert(noon.elevation_degrees < 60.0f);
        assert(noon.azimuth_degrees > 170.0f);
        assert(noon.azimuth_degrees < 190.0f);

        const double direction_length = std::sqrt(
            static_cast<double>(noon.direction.x) * noon.direction.x +
            static_cast<double>(noon.direction.y) * noon.direction.y +
            static_cast<double>(noon.direction.z) * noon.direction.z);

        assert(nearly_equal(direction_length, 1.0, 1.0e-5));

        const desktopworld::solar_state night = desktopworld::solar_model::calculate(
            make_utc_time(2024, 3, 20, 15, 0),
            latitude,
            longitude);
        assert(night.elevation_degrees < -20.0f);

        const desktopworld::solar_state summer = desktopworld::solar_model::calculate(
            make_utc_time(2024, 6, 21, 3, 0),
            latitude,
            longitude);
        const desktopworld::solar_state winter = desktopworld::solar_model::calculate(
            make_utc_time(2024, 12, 21, 3, 0),
            latitude,
            longitude);
        assert(summer.elevation_degrees > winter.elevation_degrees);

        const desktopworld::solar_state leap_day = desktopworld::solar_model::calculate(
            make_utc_time(2024, 2, 29, 3, 0),
            latitude,
            longitude);
        assert(leap_day.elevation_degrees >= -90.0f);
        assert(leap_day.elevation_degrees <= 90.0f);
    }

    void test_sample_intervals()
    {
        using desktopworld::calculate_sample_interval_seconds;

        assert(calculate_sample_interval_seconds(30.0f, 0) == 3600);
        assert(calculate_sample_interval_seconds(12.0f, 0) == 1800);
        assert(calculate_sample_interval_seconds(-6.0f, 0) == 900);
        assert(calculate_sample_interval_seconds(0.0f, 3600) == 3600);
        assert(calculate_sample_interval_seconds(0.0f, 1800) == 1800);
        assert(calculate_sample_interval_seconds(0.0f, 900) == 900);
        assert(calculate_sample_interval_seconds(0.0f, 123) == 900);
    }

    void test_sample_windows()
    {
        using desktopworld::calculate_time_sample_window;

        const desktopworld::time_sample_window start =
            calculate_time_sample_window(0, 3600);
        assert(start.start_utc_seconds == 0);
        assert(start.end_utc_seconds == 3600);
        assert(nearly_equal(start.blend_factor, 0.0, 1.0e-6));

        const desktopworld::time_sample_window middle =
            calculate_time_sample_window(1800, 3600);
        assert(middle.start_utc_seconds == 0);
        assert(middle.end_utc_seconds == 3600);
        assert(nearly_equal(middle.blend_factor, 0.5, 1.0e-6));

        const desktopworld::time_sample_window boundary =
            calculate_time_sample_window(3600, 3600);
        assert(boundary.start_utc_seconds == 3600);
        assert(boundary.end_utc_seconds == 7200);
        assert(nearly_equal(boundary.blend_factor, 0.0, 1.0e-6));

        assert(desktopworld::floor_divide(-3601, 3600) == -2);
        assert(desktopworld::floor_divide(-3600, 3600) == -1);
        assert(desktopworld::floor_divide(-3599, 3600) == -1);

        const desktopworld::time_sample_window before_epoch =
            calculate_time_sample_window(-1, 3600);
        assert(before_epoch.start_utc_seconds == -3600);
        assert(before_epoch.end_utc_seconds == 0);
        assert(before_epoch.blend_factor > 0.999f);

        bool threw = false;
        try
        {
            (void)calculate_time_sample_window(0, 0);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        assert(threw);
    }

    void test_frame_controller()
    {
        using desktopworld::frame_controller;
        using desktopworld::frame_mode;

        frame_controller controller;
        const frame_controller::clock::time_point now =
            frame_controller::clock::now();

        controller.set_mode(frame_mode::on_demand);
        assert(controller.should_render(now));
        controller.on_frame_submitted(now);
        assert(!controller.should_render(now));
        assert(controller.wait_timeout_ms(now) == 0xFFFFFFFFu);

        controller.request_frame();
        assert(controller.should_render(now));

        controller.set_fixed_fps(24.0f);
        controller.set_mode(frame_mode::fixed);
        assert(nearly_equal(controller.target_fps(), 24.0, 1.0e-5));

        controller.set_adaptive_fps(2.0f, 15.0f);
        controller.set_mode(frame_mode::adaptive);
        controller.set_solar_elevation(0.0f);
        assert(nearly_equal(controller.target_fps(), 15.0, 1.0e-5));

        controller.set_solar_elevation(30.0f);
        assert(nearly_equal(controller.target_fps(), 2.0, 1.0e-5));
    }
}

int main()
{
    test_solar_position();
    test_sample_intervals();
    test_sample_windows();
    test_frame_controller();

    std::cout << "DesktopWorld core tests passed.\n";
    return 0;
}
