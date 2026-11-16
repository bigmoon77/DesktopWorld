#pragma once

#include <Windows.h>

#include <vector>

namespace desktopworld
{
    struct display_monitor
    {
        HMONITOR handle = nullptr;
        RECT bounds{};
        RECT work_area{};
        bool primary = false;
        wchar_t device_name[CCHDEVICENAME]{};
    };

    class monitor_manager
    {
    public:
        bool refresh();

        const std::vector<display_monitor>& monitors() const;
        RECT virtual_bounds() const;
        bool is_valid() const;

    private:
        std::vector<display_monitor> _monitors;
        RECT _virtual_bounds{};
        bool _valid = false;
    };
}
