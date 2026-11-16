#include "atmosphere/reference/model.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    using atmosphere::reference::AtmosphereParameters;
    using atmosphere::reference::DensityProfileLayer;
    using atmosphere::reference::DimensionlessSpectrum;
    using atmosphere::reference::Direction;
    using atmosphere::reference::IrradianceSpectrum;
    using atmosphere::reference::Length;
    using atmosphere::reference::Model;
    using atmosphere::reference::Position;
    using atmosphere::reference::RadianceSpectrum;
    using atmosphere::reference::ScatteringSpectrum;
    using atmosphere::reference::m;
    using atmosphere::reference::rad;
    using atmosphere::reference::watt_per_square_meter_per_nm;
    using atmosphere::reference::watt_per_square_meter_per_sr_per_nm;

    constexpr unsigned int expected_width = 256;
    constexpr unsigned int expected_height = 128;
    constexpr unsigned int wavelength_count = 47;
    constexpr double pi = 3.14159265358979323846;
    constexpr double tau = 2.0 * pi;

    struct capture_metadata
    {
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int render_method = 0;
        unsigned int max_scattering_order = 0;
        double blend_factor = 0.0;
        double sun_direction_x = 0.0;
        double sun_direction_y = 1.0;
        double sun_direction_z = 0.0;

        double planet_radius_km = 6360.0;
        double atmosphere_height_km = 100.0;
        std::array<double, 3> rayleigh_scattering{};
        double rayleigh_scale_height_km = 8.0;
        std::array<double, 3> mie_scattering{};
        std::array<double, 3> mie_extinction{};
        double mie_scale_height_km = 1.2;
        double mie_phase_function_g = 0.8;
        std::array<double, 3> ozone_absorption{};
        double ozone_center_height_km = 25.0;
        double ozone_half_width_km = 15.0;
        std::array<double, 3> ground_albedo{};
        double solar_intensity = 20.0;
    };

    struct float_image
    {
        unsigned int width = 0;
        unsigned int height = 0;
        std::vector<float> rgb{};
    };

    struct channel_metrics
    {
        double mean_absolute_error = 0.0;
        double root_mean_squared_error = 0.0;
        double maximum_absolute_error = 0.0;
    };

    struct image_metrics
    {
        channel_metrics rgb[3]{};
        channel_metrics combined{};
        std::uint64_t pixel_count = 0;
    };

    bool read_key_values(
        const std::filesystem::path& path,
        std::unordered_map<std::string, std::string>& values,
        std::string& raw_text)
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return false;
        }

        std::ostringstream buffer;
        buffer << file.rdbuf();
        if (!file.good() && !file.eof())
        {
            return false;
        }

        raw_text = buffer.str();
        std::istringstream lines(raw_text);
        std::string line;
        while (std::getline(lines, line))
        {
            if (line.empty() || line.front() == '#' || line.front() == ';')
            {
                continue;
            }

            const std::size_t delimiter = line.find('=');
            if (delimiter == std::string::npos)
            {
                continue;
            }

            const std::string key = line.substr(0, delimiter);
            const std::string value = line.substr(delimiter + 1);
            values[key] = value;
        }

        return true;
    }

    bool read_number(
        const std::unordered_map<std::string, std::string>& values,
        const std::string& key,
        double& result)
    {
        const auto found = values.find(key);
        if (found == values.end())
        {
            return false;
        }

        char* end = nullptr;
        const double parsed = std::strtod(found->second.c_str(), &end);
        if (end == found->second.c_str() || *end != '\0' ||
            !std::isfinite(parsed))
        {
            return false;
        }

        result = parsed;
        return true;
    }

    bool load_metadata(
        const std::filesystem::path& path,
        capture_metadata& metadata,
        std::string& raw_text)
    {
        std::unordered_map<std::string, std::string> values;
        if (!read_key_values(path, values, raw_text))
        {
            std::cerr << "Failed to read capture metadata: " << path.string() << "\n";
            return false;
        }

        double number = 0.0;
        if (!read_number(values, "FormatVersion", number) || number != 1.0)
        {
            std::cerr << "Unsupported capture metadata version.\n";
            return false;
        }

        const auto read_unsigned = [&](const std::string& key, unsigned int& target) -> bool
        {
            double parsed = 0.0;
            if (!read_number(values, key, parsed) ||
                parsed < 0.0 ||
                parsed > static_cast<double>(std::numeric_limits<unsigned int>::max()) ||
                std::floor(parsed) != parsed)
            {
                return false;
            }

            target = static_cast<unsigned int>(parsed);
            return true;
        };

        if (!read_unsigned("Width", metadata.width) ||
            !read_unsigned("Height", metadata.height) ||
            !read_unsigned("RenderMethod", metadata.render_method) ||
            !read_unsigned("MaxScatteringOrder", metadata.max_scattering_order))
        {
            std::cerr << "Capture metadata is missing required integer fields.\n";
            return false;
        }

        const auto mapping = values.find("ViewMapping");
        if (mapping == values.end() || mapping->second != "sky_view_direction_v1")
        {
            std::cerr << "Unsupported Sky View coordinate mapping.\n";
            return false;
        }

        // The physical-model comparison must exclude temporal interpolation.
        // Render method 0 (Direct) and 1 (current Sky View) both capture the
        // current-time result. Methods 2 and 3 capture a temporally blended pair.
        if (metadata.render_method > 1)
        {
            std::cerr
                << "Select render method A (Direct) or B (Sky Current), then capture again. "
                << "C/D include temporal interpolation and cannot isolate model error.\n";
            return false;
        }

        const auto required = [&](const std::string& key, double& target) -> bool
        {
            if (!read_number(values, key, target))
            {
                std::cerr << "Capture metadata is missing or invalid: " << key << "\n";
                return false;
            }
            return true;
        };

        if (!required("BlendFactor", metadata.blend_factor) ||
            !required("SunDirectionX", metadata.sun_direction_x) ||
            !required("SunDirectionY", metadata.sun_direction_y) ||
            !required("SunDirectionZ", metadata.sun_direction_z) ||
            !required("PlanetRadiusKm", metadata.planet_radius_km) ||
            !required("AtmosphereHeightKm", metadata.atmosphere_height_km) ||
            !required("RayleighScaleHeightKm", metadata.rayleigh_scale_height_km) ||
            !required("MieScaleHeightKm", metadata.mie_scale_height_km) ||
            !required("MiePhaseFunctionG", metadata.mie_phase_function_g) ||
            !required("OzoneCenterHeightKm", metadata.ozone_center_height_km) ||
            !required("OzoneHalfWidthKm", metadata.ozone_half_width_km) ||
            !required("SolarIntensity", metadata.solar_intensity))
        {
            return false;
        }

        const std::array<std::string, 3> channel_names = { "R", "G", "B" };
        for (std::size_t channel = 0; channel < channel_names.size(); ++channel)
        {
            const std::string suffix = channel_names[channel];
            if (!required("RayleighScattering" + suffix, metadata.rayleigh_scattering[channel]) ||
                !required("MieScattering" + suffix, metadata.mie_scattering[channel]) ||
                !required("MieExtinction" + suffix, metadata.mie_extinction[channel]) ||
                !required("OzoneAbsorption" + suffix, metadata.ozone_absorption[channel]) ||
                !required("GroundAlbedo" + suffix, metadata.ground_albedo[channel]))
            {
                return false;
            }
        }

        if (metadata.width != expected_width ||
            metadata.height != expected_height ||
            metadata.max_scattering_order < 2 ||
            metadata.max_scattering_order > 4 ||
            metadata.planet_radius_km <= 0.0 ||
            metadata.atmosphere_height_km <= 0.0 ||
            metadata.rayleigh_scale_height_km <= 0.0 ||
            metadata.mie_scale_height_km <= 0.0 ||
            metadata.ozone_half_width_km <= 0.0 ||
            metadata.solar_intensity <= 0.0 ||
            std::abs(metadata.mie_phase_function_g) >= 1.0)
        {
            std::cerr << "Capture metadata contains unsupported atmosphere parameters.\n";
            return false;
        }

        const double sun_length = std::sqrt(
            metadata.sun_direction_x * metadata.sun_direction_x +
            metadata.sun_direction_y * metadata.sun_direction_y +
            metadata.sun_direction_z * metadata.sun_direction_z);
        if (sun_length < 1.0e-8)
        {
            std::cerr << "Capture metadata contains an invalid Sun direction.\n";
            return false;
        }

        metadata.sun_direction_x /= sun_length;
        metadata.sun_direction_y /= sun_length;
        metadata.sun_direction_z /= sun_length;
        return true;
    }

    bool read_pfm(
        const std::filesystem::path& path,
        float_image& image)
    {
        if (std::endian::native != std::endian::little)
        {
            std::cerr << "This PFM reader currently requires a little-endian host.\n";
            return false;
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            std::cerr << "Failed to open PFM capture: " << path.string() << "\n";
            return false;
        }

        std::string magic;
        std::string dimensions;
        std::string scale_line;
        if (!std::getline(file, magic) ||
            !std::getline(file, dimensions) ||
            !std::getline(file, scale_line) ||
            magic != "PF")
        {
            std::cerr << "Invalid PFM header.\n";
            return false;
        }

        std::istringstream dimension_stream(dimensions);
        unsigned int width = 0;
        unsigned int height = 0;
        if (!(dimension_stream >> width >> height) ||
            width != expected_width ||
            height != expected_height)
        {
            std::cerr << "PFM dimensions must be 256 x 128.\n";
            return false;
        }

        char* scale_end = nullptr;
        const double scale = std::strtod(scale_line.c_str(), &scale_end);
        if (scale_end == scale_line.c_str() || *scale_end != '\0' ||
            !std::isfinite(scale) || scale >= 0.0 || scale == 0.0)
        {
            std::cerr << "PFM must use a negative scale to indicate little-endian data.\n";
            return false;
        }

        const std::size_t component_count =
            static_cast<std::size_t>(width) * height * 3U;
        std::vector<float> file_pixels(component_count);
        const std::size_t byte_count = component_count * sizeof(float);
        file.read(
            reinterpret_cast<char*>(file_pixels.data()),
            static_cast<std::streamsize>(byte_count));
        if (file.gcount() != static_cast<std::streamsize>(byte_count))
        {
            std::cerr << "PFM pixel data is truncated.\n";
            return false;
        }

        image.width = width;
        image.height = height;
        image.rgb.resize(component_count);

        const float scale_factor = static_cast<float>(std::abs(scale));
        for (unsigned int file_y = 0; file_y < height; ++file_y)
        {
            const unsigned int image_y = height - 1U - file_y;
            for (unsigned int x = 0; x < width; ++x)
            {
                const std::size_t file_index =
                    (static_cast<std::size_t>(file_y) * width + x) * 3U;
                const std::size_t image_index =
                    (static_cast<std::size_t>(image_y) * width + x) * 3U;

                for (unsigned int channel = 0; channel < 3; ++channel)
                {
                    const float value = file_pixels[file_index + channel] * scale_factor;
                    if (!std::isfinite(value))
                    {
                        std::cerr << "PFM contains non-finite radiance values.\n";
                        return false;
                    }
                    image.rgb[image_index + channel] = value;
                }
            }
        }

        return true;
    }

    double interpolate_rgb_spectrum(
        const std::array<double, 3>& rgb,
        const double wavelength_nm)
    {
        // DesktopWorld stores RGB coefficients at B=440 nm, G=550 nm,
        // and R=680 nm. Piecewise-linear interpolation defines the other
        // reference wavelengths without changing those three channel values.
        if (wavelength_nm <= 550.0)
        {
            const double factor = (wavelength_nm - 440.0) / (550.0 - 440.0);
            return rgb[2] + (rgb[1] - rgb[2]) * factor;
        }

        const double factor = (wavelength_nm - 550.0) / (680.0 - 550.0);
        return rgb[1] + (rgb[0] - rgb[1]) * factor;
    }

    AtmosphereParameters make_reference_parameters(const capture_metadata& metadata)
    {
        AtmosphereParameters parameters{};

        std::vector<float> solar_irradiance(wavelength_count);
        std::vector<float> rayleigh_scattering(wavelength_count);
        std::vector<float> mie_scattering(wavelength_count);
        std::vector<float> mie_extinction(wavelength_count);
        std::vector<float> ozone_absorption(wavelength_count);
        std::vector<float> ground_albedo(wavelength_count);

        for (unsigned int index = 0; index < wavelength_count; ++index)
        {
            const double wavelength_nm = 360.0 + 10.0 * index;
            solar_irradiance[index] =
                static_cast<float>(metadata.solar_intensity);
            rayleigh_scattering[index] = static_cast<float>(
                interpolate_rgb_spectrum(metadata.rayleigh_scattering, wavelength_nm) /
                1000.0);
            mie_scattering[index] = static_cast<float>(
                interpolate_rgb_spectrum(metadata.mie_scattering, wavelength_nm) /
                1000.0);
            mie_extinction[index] = static_cast<float>(
                interpolate_rgb_spectrum(metadata.mie_extinction, wavelength_nm) /
                1000.0);
            ozone_absorption[index] = static_cast<float>(
                interpolate_rgb_spectrum(metadata.ozone_absorption, wavelength_nm) /
                1000.0);
            ground_albedo[index] = static_cast<float>(
                interpolate_rgb_spectrum(metadata.ground_albedo, wavelength_nm));
        }

        parameters.solar_irradiance =
            IrradianceSpectrum(solar_irradiance, watt_per_square_meter_per_nm);
        parameters.sun_angular_radius = 0.004675 * rad;
        parameters.bottom_radius = metadata.planet_radius_km * 1000.0 * m;
        parameters.top_radius =
            (metadata.planet_radius_km + metadata.atmosphere_height_km) * 1000.0 * m;

        parameters.rayleigh_density.layers[0] =
            DensityProfileLayer(0.0 * m, 0.0, 0.0 / m, 0.0 / m, 0.0);
        parameters.rayleigh_density.layers[1] =
            DensityProfileLayer(
                0.0 * m,
                1.0,
                -1.0 / (metadata.rayleigh_scale_height_km * 1000.0 * m),
                0.0 / m,
                0.0);
        parameters.rayleigh_scattering =
            ScatteringSpectrum(rayleigh_scattering, 1.0 / m);

        parameters.mie_density.layers[0] =
            DensityProfileLayer(0.0 * m, 0.0, 0.0 / m, 0.0 / m, 0.0);
        parameters.mie_density.layers[1] =
            DensityProfileLayer(
                0.0 * m,
                1.0,
                -1.0 / (metadata.mie_scale_height_km * 1000.0 * m),
                0.0 / m,
                0.0);
        parameters.mie_scattering =
            ScatteringSpectrum(mie_scattering, 1.0 / m);
        parameters.mie_extinction =
            ScatteringSpectrum(mie_extinction, 1.0 / m);
        parameters.mie_phase_function_g = metadata.mie_phase_function_g;

        const double ozone_center_m = metadata.ozone_center_height_km * 1000.0;
        const double ozone_half_width_m = metadata.ozone_half_width_km * 1000.0;
        const double ozone_base_m = ozone_center_m - ozone_half_width_m;
        parameters.absorption_density.layers[0] =
            DensityProfileLayer(
                ozone_center_m * m,
                0.0,
                0.0 / m,
                1.0 / (ozone_half_width_m * m),
                -ozone_base_m / ozone_half_width_m);
        parameters.absorption_density.layers[1] =
            DensityProfileLayer(
                0.0 * m,
                0.0,
                0.0 / m,
                -1.0 / (ozone_half_width_m * m),
                1.0 + ozone_center_m / ozone_half_width_m);
        parameters.absorption_extinction =
            ScatteringSpectrum(ozone_absorption, 1.0 / m);
        parameters.ground_albedo =
            atmosphere::reference::DimensionlessSpectrum(ground_albedo, 1.0);
        parameters.mu_s_min = -0.207912;

        return parameters;
    }

    std::uint64_t hash_text(const std::string& text)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const unsigned char value : text)
        {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    bool is_complete_reference_cache(const std::filesystem::path& directory)
    {
        const std::array<std::filesystem::path, 4> files =
        {
            directory / "transmittance.dat",
            directory / "scattering.dat",
            directory / "single_mie_scattering.dat",
            directory / "irradiance.dat"
        };

        for (const std::filesystem::path& file : files)
        {
            std::error_code error;
            if (!std::filesystem::exists(file, error) ||
                error ||
                std::filesystem::file_size(file, error) == 0 ||
                error)
            {
                return false;
            }
        }

        return true;
    }

    bool write_pfm(
        const std::filesystem::path& path,
        const float_image& image)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            std::cerr << "Failed to create reference PFM: " << path.string() << "\n";
            return false;
        }

        file << "PF\n" << image.width << " " << image.height << "\n-1.0\n";
        if (!file.good())
        {
            return false;
        }

        for (unsigned int row = image.height; row > 0; --row)
        {
            const unsigned int y = row - 1U;
            const std::size_t row_offset =
                static_cast<std::size_t>(y) * image.width * 3U;
            const std::size_t row_bytes =
                static_cast<std::size_t>(image.width) * 3U * sizeof(float);
            file.write(
                reinterpret_cast<const char*>(image.rgb.data() + row_offset),
                static_cast<std::streamsize>(row_bytes));
            if (!file.good())
            {
                return false;
            }
        }

        file.close();
        return file.good();
    }

    channel_metrics calculate_channel_metrics(
        const float_image& expected,
        const float_image& actual,
        const unsigned int channel)
    {
        double absolute_sum = 0.0;
        double squared_sum = 0.0;
        double maximum = 0.0;

        const std::size_t pixel_count =
            static_cast<std::size_t>(expected.width) * expected.height;
        for (std::size_t pixel = 0; pixel < pixel_count; ++pixel)
        {
            const std::size_t index = pixel * 3U + channel;
            const double difference = std::abs(
                static_cast<double>(expected.rgb[index]) -
                static_cast<double>(actual.rgb[index]));
            absolute_sum += difference;
            squared_sum += difference * difference;
            maximum = std::max(maximum, difference);
        }

        channel_metrics result{};
        result.mean_absolute_error = absolute_sum / static_cast<double>(pixel_count);
        result.root_mean_squared_error =
            std::sqrt(squared_sum / static_cast<double>(pixel_count));
        result.maximum_absolute_error = maximum;
        return result;
    }

    image_metrics calculate_metrics(
        const float_image& captured,
        const float_image& reference)
    {
        image_metrics result{};
        result.pixel_count =
            static_cast<std::uint64_t>(captured.width) * captured.height;

        double absolute_sum = 0.0;
        double squared_sum = 0.0;
        double maximum = 0.0;
        const std::size_t component_count = captured.rgb.size();

        for (unsigned int channel = 0; channel < 3; ++channel)
        {
            result.rgb[channel] =
                calculate_channel_metrics(captured, reference, channel);
        }

        for (std::size_t index = 0; index < component_count; ++index)
        {
            const double difference = std::abs(
                static_cast<double>(captured.rgb[index]) -
                static_cast<double>(reference.rgb[index]));
            absolute_sum += difference;
            squared_sum += difference * difference;
            maximum = std::max(maximum, difference);
        }

        result.combined.mean_absolute_error =
            absolute_sum / static_cast<double>(component_count);
        result.combined.root_mean_squared_error =
            std::sqrt(squared_sum / static_cast<double>(component_count));
        result.combined.maximum_absolute_error = maximum;
        return result;
    }

    bool write_metrics(
        const std::filesystem::path& path,
        const image_metrics& metrics,
        const capture_metadata& metadata)
    {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file.is_open())
        {
            std::cerr << "Failed to create metrics report: " << path.string() << "\n";
            return false;
        }

        file << std::setprecision(12);
        file << "Reference=Bruneton 2017 CPU reference\n";
        file << "CaptureRenderMethod=" << metadata.render_method << "\n";
        file << "ScatteringOrders=" << metadata.max_scattering_order << "\n";
        file << "Resolution=" << expected_width << "x" << expected_height << "\n";
        file << "WavelengthChannelsNm=680,550,440\n";
        file << "ReferenceSourceCommit=34f14e745cff948f4ca3157d1b62a445ffa7286f\n";
        file << "RgbCoefficientInterpolation=piecewise_linear\n";
        file << "SolarSpectrumAssumption=constant_scalar_solar_intensity\n";
        file << "CoefficientUnitConversion=per_km_to_per_m_x_0.001\n";
        file << "SunDirection=" << metadata.sun_direction_x << ","
             << metadata.sun_direction_y << "," << metadata.sun_direction_z << "\n";
        file << "PlanetRadiusKm=" << metadata.planet_radius_km << "\n";
        file << "AtmosphereHeightKm=" << metadata.atmosphere_height_km << "\n";
        file << "RayleighScatteringRGB=" << metadata.rayleigh_scattering[0] << ","
             << metadata.rayleigh_scattering[1] << "," << metadata.rayleigh_scattering[2] << "\n";
        file << "RayleighScaleHeightKm=" << metadata.rayleigh_scale_height_km << "\n";
        file << "MieScatteringRGB=" << metadata.mie_scattering[0] << ","
             << metadata.mie_scattering[1] << "," << metadata.mie_scattering[2] << "\n";
        file << "MieExtinctionRGB=" << metadata.mie_extinction[0] << ","
             << metadata.mie_extinction[1] << "," << metadata.mie_extinction[2] << "\n";
        file << "MieScaleHeightKm=" << metadata.mie_scale_height_km << "\n";
        file << "MiePhaseFunctionG=" << metadata.mie_phase_function_g << "\n";
        file << "OzoneAbsorptionRGB=" << metadata.ozone_absorption[0] << ","
             << metadata.ozone_absorption[1] << "," << metadata.ozone_absorption[2] << "\n";
        file << "OzoneCenterHeightKm=" << metadata.ozone_center_height_km << "\n";
        file << "OzoneHalfWidthKm=" << metadata.ozone_half_width_km << "\n";
        file << "GroundAlbedoRGB=" << metadata.ground_albedo[0] << ","
             << metadata.ground_albedo[1] << "," << metadata.ground_albedo[2] << "\n";
        file << "SolarIntensity=" << metadata.solar_intensity << "\n";
        file << "PixelCount=" << metrics.pixel_count << "\n";
        file << "RGBComponentMAE=" << metrics.combined.mean_absolute_error << "\n";
        file << "RGBComponentRMSE=" << metrics.combined.root_mean_squared_error << "\n";
        file << "RGBComponentMaxAbsoluteError="
             << metrics.combined.maximum_absolute_error << "\n";

        const std::array<const char*, 3> names = { "R", "G", "B" };
        for (unsigned int channel = 0; channel < 3; ++channel)
        {
            file << names[channel] << "MAE="
                 << metrics.rgb[channel].mean_absolute_error << "\n";
            file << names[channel] << "RMSE="
                 << metrics.rgb[channel].root_mean_squared_error << "\n";
            file << names[channel] << "MaxAbsoluteError="
                 << metrics.rgb[channel].maximum_absolute_error << "\n";
        }

        file.close();
        return file.good();
    }

    bool render_reference_image(
        const Model& model,
        const capture_metadata& metadata,
        float_image& image)
    {
        image.width = expected_width;
        image.height = expected_height;
        image.rgb.resize(
            static_cast<std::size_t>(expected_width) * expected_height * 3U);

        const double camera_radius_m =
            (metadata.planet_radius_km + 0.01) * 1000.0;
        const Position camera(
            0.0 * m,
            camera_radius_m * m,
            0.0 * m);
        const Direction sun_direction(
            metadata.sun_direction_x,
            metadata.sun_direction_y,
            metadata.sun_direction_z);

        for (unsigned int y = 0; y < expected_height; ++y)
        {
            for (unsigned int x = 0; x < expected_width; ++x)
            {
                const double u =
                    (static_cast<double>(x) + 0.5) / expected_width;
                const double v =
                    (static_cast<double>(y) + 0.5) / expected_height;
                const double azimuth = (u - 0.5) * tau;
                const double elevation = (1.0 - v) * (pi * 0.5);
                const double horizontal = std::cos(elevation);

                const Direction view_ray(
                    std::sin(azimuth) * horizontal,
                    std::sin(elevation),
                    std::cos(azimuth) * horizontal);

                DimensionlessSpectrum transmittance{};
                const RadianceSpectrum radiance = model.GetSkyRadiance(
                    camera,
                    view_ray,
                    0.0 * m,
                    sun_direction,
                    &transmittance);

                const std::size_t output_index =
                    (static_cast<std::size_t>(y) * expected_width + x) * 3U;
                image.rgb[output_index] = static_cast<float>(
                    radiance[32].to(watt_per_square_meter_per_sr_per_nm));
                image.rgb[output_index + 1U] = static_cast<float>(
                    radiance[19].to(watt_per_square_meter_per_sr_per_nm));
                image.rgb[output_index + 2U] = static_cast<float>(
                    radiance[8].to(watt_per_square_meter_per_sr_per_nm));

                if (!std::isfinite(image.rgb[output_index]) ||
                    !std::isfinite(image.rgb[output_index + 1U]) ||
                    !std::isfinite(image.rgb[output_index + 2U]))
                {
                    std::cerr << "Bruneton reference produced non-finite radiance.\n";
                    return false;
                }
            }
        }

        return true;
    }
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr
            << "Usage: bruneton_reference_compare <sky_capture.pfm>\n"
            << "The PFM must have a matching .meta sidecar generated by DesktopWorld.\n";
        return 2;
    }

    const std::filesystem::path capture_path =
        std::filesystem::u8path(argv[1]);
    if (!std::filesystem::exists(capture_path))
    {
        std::cerr << "Capture file does not exist.\n";
        return 2;
    }

    std::filesystem::path metadata_path = capture_path;
    metadata_path.replace_extension(L".meta");

    capture_metadata metadata{};
    std::string metadata_text;
    if (!load_metadata(metadata_path, metadata, metadata_text))
    {
        return 2;
    }

    float_image captured_image{};
    if (!read_pfm(capture_path, captured_image))
    {
        return 2;
    }

    const AtmosphereParameters parameters =
        make_reference_parameters(metadata);

    std::string atmosphere_cache_key;
    {
        std::istringstream metadata_lines(metadata_text);
        std::string line;
        while (std::getline(metadata_lines, line))
        {
            if (line.rfind("FormatVersion=", 0) == 0 ||
                line.rfind("Width=", 0) == 0 ||
                line.rfind("Height=", 0) == 0 ||
                line.rfind("ViewMapping=", 0) == 0 ||
                line.rfind("RenderMethod=", 0) == 0 ||
                line.rfind("BlendFactor=", 0) == 0 ||
                line.rfind("SunDirection", 0) == 0)
            {
                continue;
            }

            atmosphere_cache_key += line;
            atmosphere_cache_key.push_back('\n');
        }
    }

    const std::uint64_t cache_hash = hash_text(atmosphere_cache_key);
    std::ostringstream cache_name;
    cache_name << "desktopworld_bruneton_" << std::hex << cache_hash;

    std::error_code filesystem_error;
    const std::filesystem::path cache_directory =
        std::filesystem::temp_directory_path() / cache_name.str();
    if (!is_complete_reference_cache(cache_directory))
    {
        std::filesystem::remove_all(cache_directory, filesystem_error);
        filesystem_error.clear();
        std::filesystem::create_directories(cache_directory, filesystem_error);
        if (filesystem_error)
        {
            std::cerr << "Failed to create Bruneton cache directory.\n";
            return 2;
        }
    }

    std::string cache_directory_string = cache_directory.string();
    if (!cache_directory_string.empty() &&
        cache_directory_string.back() != '\\' &&
        cache_directory_string.back() != '/')
    {
        cache_directory_string.push_back(
            std::filesystem::path::preferred_separator);
    }

    std::cout
        << "Initializing the 47-wavelength Bruneton CPU reference model.\n"
        << "The first run can take several minutes; cached tables are reused.\n";

    Model reference_model(parameters, cache_directory_string);
    reference_model.Init(metadata.max_scattering_order);

    float_image reference_image{};
    if (!render_reference_image(reference_model, metadata, reference_image))
    {
        return 1;
    }

    std::filesystem::path reference_path = capture_path;
    reference_path.replace_filename(
        capture_path.stem().string() + "_bruneton_reference.pfm");
    if (!write_pfm(reference_path, reference_image))
    {
        return 1;
    }

    const image_metrics metrics =
        calculate_metrics(captured_image, reference_image);

    std::filesystem::path report_path = capture_path;
    report_path.replace_filename(
        capture_path.stem().string() + "_bruneton_metrics.txt");
    if (!write_metrics(report_path, metrics, metadata))
    {
        return 1;
    }

    std::cout << std::setprecision(10);
    std::cout << "Reference image: " << reference_path.string() << "\n";
    std::cout << "Metrics report: " << report_path.string() << "\n";
    std::cout << "RGB component MAE: " << metrics.combined.mean_absolute_error << "\n";
    std::cout << "RGB component RMSE: " << metrics.combined.root_mean_squared_error << "\n";
    std::cout << "Maximum absolute error: "
              << metrics.combined.maximum_absolute_error << "\n";
    std::cout << "Per-channel MAE (R/G/B): "
              << metrics.rgb[0].mean_absolute_error << " / "
              << metrics.rgb[1].mean_absolute_error << " / "
              << metrics.rgb[2].mean_absolute_error << "\n";

    return 0;
}
