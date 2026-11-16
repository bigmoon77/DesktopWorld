#pragma once

#include <Windows.h>

namespace desktopworld
{
    class workerw_host
    {
    public:
        HWND find_desktop_parent() const;
    };
}
