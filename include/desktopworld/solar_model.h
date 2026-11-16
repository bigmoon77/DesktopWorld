#pragma once

#include "desktopworld/types.h"
#include <Windows.h>

namespace desktopworld
{
    class solar_model
    {
    public:
        static solar_state calculate(
            const SYSTEMTIME& utc_time,
            double latitude_degrees,
            double longitude_degrees);
    };
}
