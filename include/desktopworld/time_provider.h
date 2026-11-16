#pragma once

#include <Windows.h>

namespace desktopworld
{
    class time_provider
    {
    public:
        SYSTEMTIME current_utc() const;
    };
}
