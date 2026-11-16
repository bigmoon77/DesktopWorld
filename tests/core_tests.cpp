#include "desktopworld/environment_controller.h"
#include "desktopworld/frame_controller.h"
#include "desktopworld/render_system.h"
#include "desktopworld/settings_store.h"
#include "desktopworld/solar_model.h"
#include "desktopworld/time_sample_scheduler.h"

#include <Windows.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

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

    void test_environment_controller()
    {
        desktopworld::environment_controller controller;
        controller.set_observer_location(35.6812, 139.7671);

        SYSTEMTIME utc{};
        utc.wYear = 2024;
        utc.wMonth = 3;
        utc.wDay = 20;
        utc.wHour = 3;
        utc.wMinute = 0;

        const desktopworld::environment_frame first = controller.update(utc);
        assert(first.sample_pair_changed);
        assert(first.sample_window.interval_seconds == 3600);
        assert(first.current_sun.elevation_degrees > 50.0f);

        utc.wMinute = 5;
        const desktopworld::environment_frame same_pair = controller.update(utc);
        assert(!same_pair.sample_pair_changed);
        assert(same_pair.sample_window.interval_seconds == 3600);

        controller.set_observer_location(40.7128, -74.0060);
        const desktopworld::environment_frame changed_location = controller.update(utc);
        assert(changed_location.sample_pair_changed);

        controller.set_sample_interval_override(900);
        const desktopworld::environment_frame manual_interval = controller.update(utc);
        assert(manual_interval.sample_pair_changed);
        assert(manual_interval.sample_window.interval_seconds == 900);
    }

    class test_render_pass final : public desktopworld::render_pass
    {
    public:
        test_render_pass(const int id, std::vector<int>& events)
            : _id(id), _events(&events)
        {
        }

        bool initialize() override
        {
            _events->push_back(10 + _id);
            return true;
        }

        void resize(unsigned int, unsigned int) override
        {
            _events->push_back(20 + _id);
        }

        bool execute(const desktopworld::render_context&) override
        {
            _events->push_back(30 + _id);
            return true;
        }

        void shutdown() override
        {
            _events->push_back(40 + _id);
        }

    private:
        int _id = 0;
        std::vector<int>* _events = nullptr;
    };

    void test_render_system()
    {
        std::vector<int> events;
        desktopworld::render_system renderer;

        assert(renderer.add_pass(
            std::make_unique<test_render_pass>(1, events)));
        assert(renderer.add_pass(
            std::make_unique<test_render_pass>(2, events)));
        assert(!renderer.add_pass(nullptr));
        assert(renderer.pass_count() == 2);

        assert(renderer.initialize());
        renderer.resize(1920, 1080);

        desktopworld::render_context context{};
        assert(renderer.execute(context));
        renderer.shutdown();
        assert(!renderer.is_initialized());

        const std::vector<int> expected =
        {
            11, 12,
            21, 22,
            31, 32,
            42, 41
        };
        assert(events == expected);
    }

    void test_settings_store()
    {
        wchar_t temporary_directory[MAX_PATH]{};
        const DWORD path_length = GetTempPathW(
            static_cast<DWORD>(std::size(temporary_directory)),
            temporary_directory);
        assert(path_length > 0);
        assert(path_length < std::size(temporary_directory));

        const std::filesystem::path test_directory =
            std::filesystem::path(temporary_directory) /
            (L"DesktopWorldCoreTests_" +
                std::to_wstring(GetCurrentProcessId()));

        if (!CreateDirectoryW(test_directory.c_str(), nullptr))
        {
            assert(GetLastError() == ERROR_ALREADY_EXISTS);
        }

        const std::filesystem::path settings_path =
            test_directory / L"settings.ini";
        const std::filesystem::path temporary_settings_path =
            std::filesystem::path(settings_path.native() + L".tmp");

        DeleteFileW(settings_path.c_str());
        DeleteFileW(temporary_settings_path.c_str());

        desktopworld::settings_store store(settings_path);
        desktopworld::app_settings input{};
        input.latitude = -33.8688;
        input.longitude = 151.2093;
        input.method = desktopworld::render_method::sky_blend_pixel;
        input.frame_update_mode = desktopworld::frame_mode::fixed;
        input.fixed_fps = 23.0f;
        input.sample_interval_override_seconds = 1800;
        input.atmosphere.mie_scattering.y = 0.00725f;
        input.atmosphere.mie_g = 0.87f;

        assert(store.save(input));

        desktopworld::app_settings output{};
        assert(store.load(output));
        assert(nearly_equal(output.latitude, input.latitude, 1.0e-8));
        assert(nearly_equal(output.longitude, input.longitude, 1.0e-8));
        assert(output.method == input.method);
        assert(output.frame_update_mode == input.frame_update_mode);
        assert(nearly_equal(output.fixed_fps, input.fixed_fps, 1.0e-5));
        assert(output.sample_interval_override_seconds == 1800);
        assert(nearly_equal(
            output.atmosphere.mie_scattering.y,
            input.atmosphere.mie_scattering.y,
            1.0e-6));
        assert(nearly_equal(
            output.atmosphere.mie_g,
            input.atmosphere.mie_g,
            1.0e-6));

        DeleteFileW(settings_path.c_str());
        DeleteFileW(temporary_settings_path.c_str());
        RemoveDirectoryW(test_directory.c_str());
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
    test_environment_controller();
    test_render_system();
    test_settings_store();
    test_frame_controller();

    std::cout << "DesktopWorld core tests passed.\n";
    return 0;
}
