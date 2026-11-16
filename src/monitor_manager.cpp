#include "desktopworld/monitor_manager.h"

#include <algorithm>

namespace desktopworld
{
    namespace
    {
        struct enumeration_context
        {
            std::vector<display_monitor>* monitors = nullptr;
        };

        BOOL CALLBACK enumerate_monitor(
            HMONITOR monitor,
            HDC,
            LPRECT,
            const LPARAM parameter)
        {
            enumeration_context* context =
                reinterpret_cast<enumeration_context*>(parameter);

            if (context == nullptr || context->monitors == nullptr)
            {
                return FALSE;
            }

            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info))
            {
                return TRUE;
            }

            display_monitor result{};
            result.handle = monitor;
            result.bounds = info.rcMonitor;
            result.work_area = info.rcWork;
            result.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
            std::copy_n(
                info.szDevice,
                CCHDEVICENAME,
                result.device_name);

            context->monitors->push_back(result);
            return TRUE;
        }
    }

    bool monitor_manager::refresh()
    {
        std::vector<display_monitor> monitors;
        enumeration_context context{};
        context.monitors = &monitors;

        if (!EnumDisplayMonitors(
            nullptr,
            nullptr,
            enumerate_monitor,
            reinterpret_cast<LPARAM>(&context)))
        {
            _valid = false;
            _monitors.clear();
            return false;
        }

        if (monitors.empty())
        {
            _valid = false;
            _monitors.clear();
            return false;
        }

        RECT bounds = monitors.front().bounds;
        for (const display_monitor& monitor : monitors)
        {
            bounds.left = std::min(bounds.left, monitor.bounds.left);
            bounds.top = std::min(bounds.top, monitor.bounds.top);
            bounds.right = std::max(bounds.right, monitor.bounds.right);
            bounds.bottom = std::max(bounds.bottom, monitor.bounds.bottom);
        }

        std::sort(
            monitors.begin(),
            monitors.end(),
            [](const display_monitor& left, const display_monitor& right)
            {
                if (left.primary != right.primary)
                {
                    return left.primary;
                }

                if (left.bounds.left != right.bounds.left)
                {
                    return left.bounds.left < right.bounds.left;
                }

                return left.bounds.top < right.bounds.top;
            });

        _monitors = std::move(monitors);
        _virtual_bounds = bounds;
        _valid = true;
        return true;
    }

    const std::vector<display_monitor>& monitor_manager::monitors() const
    {
        return _monitors;
    }

    RECT monitor_manager::virtual_bounds() const
    {
        return _virtual_bounds;
    }

    bool monitor_manager::is_valid() const
    {
        return _valid;
    }
}
