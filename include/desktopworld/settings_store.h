#pragma once

#include "desktopworld/types.h"

#include <Windows.h>
#include <filesystem>

namespace desktopworld
{
    struct app_settings
    {
        double latitude = 35.6812;
        double longitude = 139.7671;
        double altitude_meters = 0.0;

        render_method method = render_method::sky_blend_compute;
        frame_mode frame_update_mode = frame_mode::adaptive;
        float base_fps = 2.0f;
        float fixed_fps = 2.0f;
        float twilight_fps = 15.0f;
        int sample_interval_override_seconds = 0;

        atmosphere_parameters atmosphere{};
    };

    class settings_store
    {
    public:
        bool load(app_settings& settings);
        bool save(const app_settings& settings) const;

        const std::filesystem::path& path() const;

    private:
        bool resolve_path() const;
        double read_double(
            const wchar_t* section,
            const wchar_t* key,
            double fallback) const;
        int read_integer(
            const wchar_t* section,
            const wchar_t* key,
            int fallback) const;
        bool write_string(
            const wchar_t* section,
            const wchar_t* key,
            const std::wstring& value) const;
        static std::wstring format_double(double value);

        mutable std::filesystem::path _path;
    };
}
