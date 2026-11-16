#include "desktopworld/settings_store.h"

#include <ShlObj.h>

#include <algorithm>
#include <climits>
#include <iterator>
#include <cmath>
#include <cwchar>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace desktopworld
{
    namespace
    {
        constexpr wchar_t settings_directory_name[] = L"DesktopWorld";
        constexpr wchar_t settings_file_name[] = L"settings.ini";

        bool is_valid_method(const int value)
        {
            return value >= static_cast<int>(render_method::direct) &&
                value <= static_cast<int>(render_method::sky_blend_compute);
        }

        bool is_valid_frame_mode(const int value)
        {
            return value >= static_cast<int>(frame_mode::adaptive) &&
                value <= static_cast<int>(frame_mode::on_demand);
        }

        int validate_sample_interval(const int value)
        {
            if (value == 0 || value == 900 || value == 1800 || value == 3600)
            {
                return value;
            }
            return 0;
        }
    }

    bool settings_store::load(app_settings& settings)
    {
        settings = app_settings{};

        if (!resolve_path())
        {
            return false;
        }

        const DWORD attributes = GetFileAttributesW(_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (!save(settings))
            {
                return false;
            }
        }

        settings.latitude = std::clamp(
            read_double(L"Location", L"Latitude", settings.latitude),
            -90.0,
            90.0);
        settings.longitude = std::clamp(
            read_double(L"Location", L"Longitude", settings.longitude),
            -180.0,
            180.0);
        settings.altitude_meters = std::clamp(
            read_double(L"Location", L"AltitudeMeters", settings.altitude_meters),
            -500.0,
            10000.0);

        const int method = read_integer(
            L"Rendering", L"Method", static_cast<int>(settings.method));
        if (is_valid_method(method))
        {
            settings.method = static_cast<render_method>(method);
        }

        const int frame_mode_value = read_integer(
            L"Rendering",
            L"FrameMode",
            static_cast<int>(settings.frame_update_mode));
        if (is_valid_frame_mode(frame_mode_value))
        {
            settings.frame_update_mode =
                static_cast<frame_mode>(frame_mode_value);
        }

        settings.base_fps = static_cast<float>(std::clamp(
            read_double(L"Rendering", L"BaseFps", settings.base_fps),
            1.0,
            30.0));
        settings.fixed_fps = static_cast<float>(std::clamp(
            read_double(L"Rendering", L"FixedFps", settings.fixed_fps),
            1.0,
            30.0));
        settings.twilight_fps = static_cast<float>(std::clamp(
            read_double(L"Rendering", L"TwilightFps", settings.twilight_fps),
            static_cast<double>(settings.base_fps),
            30.0));

        settings.sample_interval_override_seconds = validate_sample_interval(
            read_integer(
                L"Rendering",
                L"SampleIntervalMode",
                settings.sample_interval_override_seconds));

        settings.atmosphere.planet_radius_km = static_cast<float>(std::clamp(
            read_double(
                L"Atmosphere",
                L"PlanetRadiusKm",
                settings.atmosphere.planet_radius_km),
            1000.0,
            10000.0));
        settings.atmosphere.atmosphere_height_km = static_cast<float>(std::clamp(
            read_double(
                L"Atmosphere",
                L"AtmosphereHeightKm",
                settings.atmosphere.atmosphere_height_km),
            1.0,
            500.0));

        settings.atmosphere.rayleigh_scattering.x = static_cast<float>(std::clamp(
            read_double(L"Atmosphere", L"RayleighR", settings.atmosphere.rayleigh_scattering.x),
            0.0,
            0.1));
        settings.atmosphere.rayleigh_scattering.y = static_cast<float>(std::clamp(
            read_double(L"Atmosphere", L"RayleighG", settings.atmosphere.rayleigh_scattering.y),
            0.0,
            0.1));
        settings.atmosphere.rayleigh_scattering.z = static_cast<float>(std::clamp(
            read_double(L"Atmosphere", L"RayleighB", settings.atmosphere.rayleigh_scattering.z),
            0.0,
            0.1));
        settings.atmosphere.mie_scattering.x = static_cast<float>(std::clamp(
            read_double(L"Atmosphere", L"MieR", settings.atmosphere.mie_scattering.x),
            0.0,
            0.1));
        settings.atmosphere.mie_scattering.y = static_cast<float>(std::clamp(
            read_double(L"Atmosphere", L"MieG", settings.atmosphere.mie_scattering.y),
            0.0,
            0.1));
        settings.atmosphere.mie_scattering.z = static_cast<float>(std::clamp(
            read_double(L"Atmosphere", L"MieB", settings.atmosphere.mie_scattering.z),
            0.0,
            0.1));

        settings.atmosphere.rayleigh_scale_height_km = static_cast<float>(std::clamp(
            read_double(
                L"Atmosphere",
                L"RayleighScaleHeightKm",
                settings.atmosphere.rayleigh_scale_height_km),
            0.1,
            100.0));
        settings.atmosphere.mie_scale_height_km = static_cast<float>(std::clamp(
            read_double(
                L"Atmosphere",
                L"MieScaleHeightKm",
                settings.atmosphere.mie_scale_height_km),
            0.1,
            50.0));
        settings.atmosphere.mie_g = static_cast<float>(std::clamp(
            read_double(L"Atmosphere", L"MieG", settings.atmosphere.mie_g),
            -0.98,
            0.98));
        settings.atmosphere.solar_intensity = static_cast<float>(std::clamp(
            read_double(
                L"Atmosphere",
                L"SolarIntensity",
                settings.atmosphere.solar_intensity),
            0.0,
            1000.0));
        settings.atmosphere.exposure = static_cast<float>(std::clamp(
            read_double(
                L"Rendering",
                L"Exposure",
                settings.atmosphere.exposure),
            0.01,
            16.0));

        return true;
    }

    bool settings_store::save(const app_settings& settings) const
    {
        if (!resolve_path())
        {
            return false;
        }

        bool success = true;
        const auto write_double = [&](const wchar_t* section,
            const wchar_t* key,
            const double value)
        {
            success = write_string(section, key, format_double(value)) && success;
        };
        const auto write_integer = [&](const wchar_t* section,
            const wchar_t* key,
            const int value)
        {
            success = write_string(section, key, std::to_wstring(value)) && success;
        };

        write_double(L"Location", L"Latitude", settings.latitude);
        write_double(L"Location", L"Longitude", settings.longitude);
        write_double(L"Location", L"AltitudeMeters", settings.altitude_meters);

        write_integer(L"Rendering", L"Method", static_cast<int>(settings.method));
        write_integer(
            L"Rendering",
            L"FrameMode",
            static_cast<int>(settings.frame_update_mode));
        write_double(L"Rendering", L"BaseFps", settings.base_fps);
        write_double(L"Rendering", L"FixedFps", settings.fixed_fps);
        write_double(L"Rendering", L"TwilightFps", settings.twilight_fps);
        write_integer(
            L"Rendering",
            L"SampleIntervalMode",
            settings.sample_interval_override_seconds);
        write_double(L"Rendering", L"Exposure", settings.atmosphere.exposure);

        write_double(
            L"Atmosphere",
            L"PlanetRadiusKm",
            settings.atmosphere.planet_radius_km);
        write_double(
            L"Atmosphere",
            L"AtmosphereHeightKm",
            settings.atmosphere.atmosphere_height_km);
        write_double(
            L"Atmosphere",
            L"RayleighR",
            settings.atmosphere.rayleigh_scattering.x);
        write_double(
            L"Atmosphere",
            L"RayleighG",
            settings.atmosphere.rayleigh_scattering.y);
        write_double(
            L"Atmosphere",
            L"RayleighB",
            settings.atmosphere.rayleigh_scattering.z);
        write_double(
            L"Atmosphere",
            L"MieR",
            settings.atmosphere.mie_scattering.x);
        write_double(
            L"Atmosphere",
            L"MieG",
            settings.atmosphere.mie_scattering.y);
        write_double(
            L"Atmosphere",
            L"MieB",
            settings.atmosphere.mie_scattering.z);
        write_double(
            L"Atmosphere",
            L"RayleighScaleHeightKm",
            settings.atmosphere.rayleigh_scale_height_km);
        write_double(
            L"Atmosphere",
            L"MieScaleHeightKm",
            settings.atmosphere.mie_scale_height_km);
        write_double(
            L"Atmosphere",
            L"MieG",
            settings.atmosphere.mie_g);
        write_double(
            L"Atmosphere",
            L"SolarIntensity",
            settings.atmosphere.solar_intensity);

        return success;
    }

    const std::filesystem::path& settings_store::path() const
    {
        return _path;
    }

    bool settings_store::resolve_path() const
    {
        if (!_path.empty())
        {
            return true;
        }

        wchar_t app_data[MAX_PATH]{};
        const HRESULT result = SHGetFolderPathW(
            nullptr,
            CSIDL_APPDATA | CSIDL_FLAG_CREATE,
            nullptr,
            SHGFP_TYPE_CURRENT,
            app_data);

        if (FAILED(result))
        {
            return false;
        }

        const std::filesystem::path directory =
            std::filesystem::path(app_data) / settings_directory_name;

        if (!CreateDirectoryW(directory.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
        {
            return false;
        }

        _path = directory / settings_file_name;
        return true;
    }

    double settings_store::read_double(
        const wchar_t* section,
        const wchar_t* key,
        const double fallback) const
    {
        wchar_t value[128]{};
        GetPrivateProfileStringW(
            section,
            key,
            L"",
            value,
            static_cast<DWORD>(std::size(value)),
            _path.c_str());

        if (value[0] == L'\0')
        {
            return fallback;
        }

        wchar_t* end = nullptr;
        const double parsed = std::wcstod(value, &end);
        if (end == value || !std::isfinite(parsed))
        {
            return fallback;
        }

        return parsed;
    }

    int settings_store::read_integer(
        const wchar_t* section,
        const wchar_t* key,
        const int fallback) const
    {
        wchar_t value[64]{};
        GetPrivateProfileStringW(
            section,
            key,
            L"",
            value,
            static_cast<DWORD>(std::size(value)),
            _path.c_str());

        if (value[0] == L'\0')
        {
            return fallback;
        }

        wchar_t* end = nullptr;
        const long parsed = std::wcstol(value, &end, 10);
        if (end == value ||
            parsed < static_cast<long>(INT_MIN) ||
            parsed > static_cast<long>(INT_MAX))
        {
            return fallback;
        }

        return static_cast<int>(parsed);
    }

    bool settings_store::write_string(
        const wchar_t* section,
        const wchar_t* key,
        const std::wstring& value) const
    {
        return WritePrivateProfileStringW(
            section,
            key,
            value.c_str(),
            _path.c_str()) != FALSE;
    }

    std::wstring settings_store::format_double(const double value)
    {
        std::wostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(10) << value;
        return stream.str();
    }
}
