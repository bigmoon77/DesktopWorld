#pragma once

#include <Windows.h>

#include <string>

namespace desktopworld
{
    struct desktop_host_layout
    {
        HWND parent_window = nullptr;
        HWND shell_view_window = nullptr;
        HWND wallpaper_window = nullptr;
        bool uses_progman_layered_layout = false;
    };

    class workerw_host
    {
    public:
        bool find_desktop_parent(
            desktop_host_layout& layout,
            std::wstring* diagnostics = nullptr) const;
    };
}
