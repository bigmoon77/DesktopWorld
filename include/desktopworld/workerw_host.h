#pragma once

#include <Windows.h>

#include <string>

namespace desktopworld
{
    class workerw_host
    {
    public:
        HWND find_desktop_parent(std::wstring* diagnostics = nullptr) const;
    };
}
