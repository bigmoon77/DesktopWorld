#include "desktopworld/solar_model.h"

#include <algorithm>
#include <cmath>

namespace desktopworld
{
    namespace
    {
        constexpr double pi = 3.14159265358979323846;

        double degrees_to_radians(const double degrees)
        {
            return degrees * pi / 180.0;
        }

        double radians_to_degrees(const double radians)
        {
            return radians * 180.0 / pi;
        }

        int day_of_year(const SYSTEMTIME& time)
        {
            constexpr int month_days[12] =
            {
                31, 28, 31, 30, 31, 30,
                31, 31, 30, 31, 30, 31
            };

            const bool leap_year =
                (time.wYear % 4 == 0 && time.wYear % 100 != 0) ||
                (time.wYear % 400 == 0);

            int result = static_cast<int>(time.wDay);
            for (int month = 1; month < static_cast<int>(time.wMonth); ++month)
            {
                result += month_days[month - 1];
                if (month == 2 && leap_year)
                {
                    ++result;
                }
            }

            return result;
        }

        double normalize_degrees(double degrees)
        {
            while (degrees < 0.0)
            {
                degrees += 360.0;
            }
            while (degrees >= 360.0)
            {
                degrees -= 360.0;
            }
            return degrees;
        }
    }

    solar_state solar_model::calculate(
        const SYSTEMTIME& utc_time,
        const double latitude_degrees,
        const double longitude_degrees)
    {
        const double latitude = degrees_to_radians(
            std::clamp(latitude_degrees, -90.0, 90.0));

        const double day = static_cast<double>(day_of_year(utc_time));
        const double fractional_hour =
            static_cast<double>(utc_time.wHour) +
            static_cast<double>(utc_time.wMinute) / 60.0 +
            static_cast<double>(utc_time.wSecond) / 3600.0;

        const double gamma = (2.0 * pi / 365.0) *
            (day - 1.0 + (fractional_hour - 12.0) / 24.0);

        const double equation_of_time = 229.18 *
            (0.000075 +
             0.001868 * std::cos(gamma) -
             0.032077 * std::sin(gamma) -
             0.014615 * std::cos(2.0 * gamma) -
             0.040849 * std::sin(2.0 * gamma));

        const double declination =
            0.006918 -
            0.399912 * std::cos(gamma) +
            0.070257 * std::sin(gamma) -
            0.006758 * std::cos(2.0 * gamma) +
            0.000907 * std::sin(2.0 * gamma) -
            0.002697 * std::cos(3.0 * gamma) +
            0.001480 * std::sin(3.0 * gamma);

        const double true_solar_minutes =
            fractional_hour * 60.0 +
            equation_of_time +
            4.0 * longitude_degrees;

        const double hour_angle = degrees_to_radians(
            true_solar_minutes / 4.0 - 180.0);

        const double cosine_zenith = std::clamp(
            std::sin(latitude) * std::sin(declination) +
            std::cos(latitude) * std::cos(declination) * std::cos(hour_angle),
            -1.0,
            1.0);

        const double zenith = std::acos(cosine_zenith);
        const double elevation = pi * 0.5 - zenith;

        double azimuth = std::atan2(
            std::sin(hour_angle),
            std::cos(hour_angle) * std::sin(latitude) -
            std::tan(declination) * std::cos(latitude));

        azimuth = normalize_degrees(radians_to_degrees(azimuth) + 180.0);
        const double azimuth_radians = degrees_to_radians(azimuth);

        const double horizontal = std::cos(elevation);

        solar_state result{};
        result.direction =
        {
            static_cast<float>(horizontal * std::sin(azimuth_radians)),
            static_cast<float>(std::sin(elevation)),
            static_cast<float>(horizontal * std::cos(azimuth_radians))
        };
        result.elevation_degrees = static_cast<float>(radians_to_degrees(elevation));
        result.azimuth_degrees = static_cast<float>(azimuth);
        return result;
    }
}
