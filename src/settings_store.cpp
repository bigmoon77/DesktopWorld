#include "desktopworld/settings_store.h"

#include <ShlObj.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>

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

        std::string trim(const std::string& value)
        {
            const auto first = std::find_if_not(
                value.begin(),
                value.end(),
                [](const unsigned char character)
                {
                    return std::isspace(character) != 0;
                });

            if (first == value.end())
            {
                return {};
            }

            const auto last = std::find_if_not(
                value.rbegin(),
                value.rend(),
                [](const unsigned char character)
                {
                    return std::isspace(character) != 0;
                }).base();

            return std::string(first, last);
        }

        std::string format_double(const double value)
        {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << std::setprecision(10) << value;
            return stream.str();
        }
    }

    settings_store::settings_store(std::filesystem::path path)
        : _path(std::move(path))
    {
    }

    bool settings_store::load(app_settings& settings)
    {
        settings = app_settings{};

        if (!resolve_path())
        {
            return false;
        }

        if (GetFileAttributesW(_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            if (!save(settings))
            {
                return false;
            }
        }

        std::ifstream file(_path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        _values.clear();
        std::string current_section;
        std::string line;
        bool first_line = true;

        while (std::getline(file, line))
        {
            if (first_line)
            {
                first_line = false;
                if (line.size() >= 3 &&
                    static_cast<unsigned char>(line[0]) == 0xEF &&
                    static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                {
                    line.erase(0, 3);
                }
            }

            const std::string trimmed_line = trim(line);
            if (trimmed_line.empty() ||
                trimmed_line.front() == ';' ||
                trimmed_line.front() == '#')
            {
                continue;
            }

            if (trimmed_line.front() == '[' &&
                trimmed_line.back() == ']')
            {
                current_section = trim(
                    trimmed_line.substr(1, trimmed_line.size() - 2));
                continue;
            }

            const std::size_t separator = trimmed_line.find('=');
            if (separator == std::string::npos || current_section.empty())
            {
                continue;
            }

            const std::string key = trim(trimmed_line.substr(0, separator));
            const std::string value = trim(trimmed_line.substr(separator + 1));
            if (!key.empty())
            {
                _values[current_section][key] = value;
            }
        }

        if (file.bad())
        {
            return false;
        }

        settings.latitude = std::clamp(
            read_double("Location", "Latitude", settings.latitude),
            -90.0,
            90.0);
        settings.longitude = std::clamp(
            read_double("Location", "Longitude", settings.longitude),
            -180.0,
            180.0);

        const int method = read_integer(
            "Rendering", "Method", static_cast<int>(settings.method));
        if (is_valid_method(method))
        {
            settings.method = static_cast<render_method>(method);
        }

        const int frame_mode_value = read_integer(
            "Rendering",
            "FrameMode",
            static_cast<int>(settings.frame_update_mode));
        if (is_valid_frame_mode(frame_mode_value))
        {
            settings.frame_update_mode =
                static_cast<frame_mode>(frame_mode_value);
        }

        settings.base_fps = static_cast<float>(std::clamp(
            read_double("Rendering", "BaseFps", settings.base_fps),
            1.0,
            30.0));
        settings.fixed_fps = static_cast<float>(std::clamp(
            read_double("Rendering", "FixedFps", settings.fixed_fps),
            1.0,
            30.0));
        settings.twilight_fps = static_cast<float>(std::clamp(
            read_double("Rendering", "TwilightFps", settings.twilight_fps),
            static_cast<double>(settings.base_fps),
            30.0));

        settings.sample_interval_override_seconds = validate_sample_interval(
            read_integer(
                "Rendering",
                "SampleIntervalMode",
                settings.sample_interval_override_seconds));

        settings.atmosphere.planet_radius_km = static_cast<float>(std::clamp(
            read_double(
                "Atmosphere",
                "PlanetRadiusKm",
                settings.atmosphere.planet_radius_km),
            1000.0,
            10000.0));
        settings.atmosphere.atmosphere_height_km = static_cast<float>(std::clamp(
            read_double(
                "Atmosphere",
                "AtmosphereHeightKm",
                settings.atmosphere.atmosphere_height_km),
            1.0,
            500.0));

        settings.atmosphere.rayleigh_scattering.x = static_cast<float>(std::clamp(
            read_double("Atmosphere", "RayleighR", settings.atmosphere.rayleigh_scattering.x),
            0.0,
            0.1));
        settings.atmosphere.rayleigh_scattering.y = static_cast<float>(std::clamp(
            read_double("Atmosphere", "RayleighG", settings.atmosphere.rayleigh_scattering.y),
            0.0,
            0.1));
        settings.atmosphere.rayleigh_scattering.z = static_cast<float>(std::clamp(
            read_double("Atmosphere", "RayleighB", settings.atmosphere.rayleigh_scattering.z),
            0.0,
            0.1));
        settings.atmosphere.mie_scattering.x = static_cast<float>(std::clamp(
            read_double("Atmosphere", "MieScatteringR", settings.atmosphere.mie_scattering.x),
            0.0,
            0.1));
        settings.atmosphere.mie_scattering.y = static_cast<float>(std::clamp(
            read_double("Atmosphere", "MieScatteringG", settings.atmosphere.mie_scattering.y),
            0.0,
            0.1));
        settings.atmosphere.mie_scattering.z = static_cast<float>(std::clamp(
            read_double("Atmosphere", "MieScatteringB", settings.atmosphere.mie_scattering.z),
            0.0,
            0.1));

        settings.atmosphere.rayleigh_scale_height_km = static_cast<float>(std::clamp(
            read_double(
                "Atmosphere",
                "RayleighScaleHeightKm",
                settings.atmosphere.rayleigh_scale_height_km),
            0.1,
            100.0));
        settings.atmosphere.mie_scale_height_km = static_cast<float>(std::clamp(
            read_double(
                "Atmosphere",
                "MieScaleHeightKm",
                settings.atmosphere.mie_scale_height_km),
            0.1,
            50.0));
        settings.atmosphere.mie_g = static_cast<float>(std::clamp(
            read_double("Atmosphere", "MieAnisotropyG", settings.atmosphere.mie_g),
            -0.98,
            0.98));
        settings.atmosphere.solar_intensity = static_cast<float>(std::clamp(
            read_double(
                "Atmosphere",
                "SolarIntensity",
                settings.atmosphere.solar_intensity),
            0.0,
            1000.0));
        settings.atmosphere.exposure = static_cast<float>(std::clamp(
            read_double(
                "Rendering",
                "Exposure",
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

        std::filesystem::path temporary_path = _path;
        temporary_path += L".tmp";

        std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return false;
        }

        file.imbue(std::locale::classic());

        file << "[Location]\n";
        file << "Latitude=" << format_double(settings.latitude) << "\n";
        file << "Longitude=" << format_double(settings.longitude) << "\n\n";

        file << "[Rendering]\n";
        file << "Method=" << static_cast<int>(settings.method) << "\n";
        file << "FrameMode=" << static_cast<int>(settings.frame_update_mode) << "\n";
        file << "BaseFps=" << format_double(settings.base_fps) << "\n";
        file << "FixedFps=" << format_double(settings.fixed_fps) << "\n";
        file << "TwilightFps=" << format_double(settings.twilight_fps) << "\n";
        file << "SampleIntervalMode="
            << settings.sample_interval_override_seconds << "\n";
        file << "Exposure=" << format_double(settings.atmosphere.exposure) << "\n\n";

        file << "[Atmosphere]\n";
        file << "PlanetRadiusKm="
            << format_double(settings.atmosphere.planet_radius_km) << "\n";
        file << "AtmosphereHeightKm="
            << format_double(settings.atmosphere.atmosphere_height_km) << "\n";
        file << "RayleighR="
            << format_double(settings.atmosphere.rayleigh_scattering.x) << "\n";
        file << "RayleighG="
            << format_double(settings.atmosphere.rayleigh_scattering.y) << "\n";
        file << "RayleighB="
            << format_double(settings.atmosphere.rayleigh_scattering.z) << "\n";
        file << "MieScatteringR="
            << format_double(settings.atmosphere.mie_scattering.x) << "\n";
        file << "MieScatteringG="
            << format_double(settings.atmosphere.mie_scattering.y) << "\n";
        file << "MieScatteringB="
            << format_double(settings.atmosphere.mie_scattering.z) << "\n";
        file << "RayleighScaleHeightKm="
            << format_double(settings.atmosphere.rayleigh_scale_height_km) << "\n";
        file << "MieScaleHeightKm="
            << format_double(settings.atmosphere.mie_scale_height_km) << "\n";
        file << "MieAnisotropyG="
            << format_double(settings.atmosphere.mie_g) << "\n";
        file << "SolarIntensity="
            << format_double(settings.atmosphere.solar_intensity) << "\n";

        file.flush();
        if (!file.good())
        {
            file.close();
            DeleteFileW(temporary_path.c_str());
            return false;
        }

        file.close();
        if (file.fail())
        {
            DeleteFileW(temporary_path.c_str());
            return false;
        }

        if (!MoveFileExW(
            temporary_path.c_str(),
            _path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temporary_path.c_str());
            return false;
        }

        return true;
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
        const char* section,
        const char* key,
        const double fallback) const
    {
        const auto section_iterator = _values.find(section);
        if (section_iterator == _values.end())
        {
            return fallback;
        }

        const auto value_iterator = section_iterator->second.find(key);
        if (value_iterator == section_iterator->second.end())
        {
            return fallback;
        }

        std::istringstream stream(value_iterator->second);
        stream.imbue(std::locale::classic());

        double parsed = 0.0;
        stream >> parsed;
        if (stream.fail())
        {
            return fallback;
        }

        stream >> std::ws;
        if (stream.peek() != std::char_traits<char>::eof() ||
            !std::isfinite(parsed))
        {
            return fallback;
        }

        return parsed;
    }

    int settings_store::read_integer(
        const char* section,
        const char* key,
        const int fallback) const
    {
        const auto section_iterator = _values.find(section);
        if (section_iterator == _values.end())
        {
            return fallback;
        }

        const auto value_iterator = section_iterator->second.find(key);
        if (value_iterator == section_iterator->second.end())
        {
            return fallback;
        }

        std::istringstream stream(value_iterator->second);
        stream.imbue(std::locale::classic());

        long long parsed = 0;
        stream >> parsed;
        if (stream.fail())
        {
            return fallback;
        }

        stream >> std::ws;
        if (stream.peek() != std::char_traits<char>::eof() ||
            parsed < static_cast<long long>(INT_MIN) ||
            parsed > static_cast<long long>(INT_MAX))
        {
            return fallback;
        }

        return static_cast<int>(parsed);
    }
}
