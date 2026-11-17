#include "desktopworld/workerw_host.h"

#include <sstream>
#include <string>

namespace desktopworld
{
    namespace
    {
        void append_diagnostic(
            std::wstring* diagnostics,
            const std::wstring& message)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            if (!diagnostics->empty())
            {
                diagnostics->append(L" | ");
            }
            diagnostics->append(message);
        }

        std::wstring format_window_handle(const HWND window)
        {
            std::wostringstream stream;
            stream << L"0x"
                << std::hex
                << std::uppercase
                << reinterpret_cast<UINT_PTR>(window);
            return stream.str();
        }

        bool find_progman_layered_layout(
            const HWND program_manager,
            desktop_host_layout& layout,
            std::wstring* diagnostics)
        {
            if (program_manager == nullptr)
            {
                return false;
            }

            const HWND shell_view = FindWindowExW(
                program_manager,
                nullptr,
                L"SHELLDLL_DefView",
                nullptr);

            if (shell_view == nullptr)
            {
                append_diagnostic(
                    diagnostics,
                    L"Progman has no direct SHELLDLL_DefView child");
                return false;
            }

            HWND wallpaper = FindWindowExW(
                program_manager,
                shell_view,
                L"WorkerW",
                nullptr);

            if (wallpaper == nullptr)
            {
                wallpaper = FindWindowExW(
                    program_manager,
                    nullptr,
                    L"WorkerW",
                    nullptr);
            }

            if (wallpaper == nullptr)
            {
                append_diagnostic(
                    diagnostics,
                    L"Progman has SHELLDLL_DefView but no child WorkerW");
                return false;
            }

            layout.parent_window = program_manager;
            layout.shell_view_window = shell_view;
            layout.wallpaper_window = wallpaper;
            layout.uses_progman_layered_layout = true;

            append_diagnostic(
                diagnostics,
                L"detected layered Progman layout: parent=" +
                format_window_handle(program_manager) +
                L", SHELLDLL_DefView=" +
                format_window_handle(shell_view) +
                L", wallpaper WorkerW=" +
                format_window_handle(wallpaper));

            return true;
        }

        bool find_legacy_desktop_parent(
            desktop_host_layout& layout,
            std::wstring* diagnostics)
        {
            struct search_context
            {
                desktop_host_layout layout{};
                unsigned int shell_view_parent_count = 0;
                unsigned int sibling_worker_count = 0;
            } context;

            SetLastError(ERROR_SUCCESS);
            const BOOL enumeration_succeeded = EnumWindows(
                [](HWND window, const LPARAM parameter) -> BOOL
                {
                    search_context* search =
                        reinterpret_cast<search_context*>(parameter);

                    const HWND shell_view = FindWindowExW(
                        window,
                        nullptr,
                        L"SHELLDLL_DefView",
                        nullptr);

                    if (shell_view == nullptr)
                    {
                        return TRUE;
                    }

                    ++search->shell_view_parent_count;

                    const HWND sibling_worker = FindWindowExW(
                        nullptr,
                        window,
                        L"WorkerW",
                        nullptr);

                    if (sibling_worker != nullptr)
                    {
                        ++search->sibling_worker_count;
                        if (search->layout.parent_window == nullptr)
                        {
                            search->layout.parent_window = sibling_worker;
                        }
                    }

                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&context));

            append_diagnostic(
                diagnostics,
                L"legacy scan: SHELLDLL_DefView parents=" +
                std::to_wstring(context.shell_view_parent_count) +
                L", following top-level WorkerW=" +
                std::to_wstring(context.sibling_worker_count));

            if (!enumeration_succeeded)
            {
                append_diagnostic(
                    diagnostics,
                    L"EnumWindows failed with Win32 error " +
                    std::to_wstring(GetLastError()));
            }

            if (context.layout.parent_window == nullptr)
            {
                return false;
            }

            layout = context.layout;
            append_diagnostic(
                diagnostics,
                L"detected legacy WorkerW parent=" +
                format_window_handle(layout.parent_window));
            return true;
        }
    }

    bool workerw_host::find_desktop_parent(
        desktop_host_layout& layout,
        std::wstring* diagnostics) const
    {
        layout = {};
        if (diagnostics != nullptr)
        {
            diagnostics->clear();
        }

        const HWND program_manager = FindWindowW(L"Progman", nullptr);
        if (program_manager == nullptr)
        {
            append_diagnostic(
                diagnostics,
                L"Progman was not found");
        }
        else
        {
            append_diagnostic(
                diagnostics,
                L"Progman=" + format_window_handle(program_manager));

            if (find_progman_layered_layout(
                program_manager,
                layout,
                diagnostics))
            {
                return true;
            }
        }

        // Older Explorer layouts use a top-level WorkerW sibling after
        // the window that owns SHELLDLL_DefView.
        if (find_legacy_desktop_parent(layout, diagnostics))
        {
            return true;
        }

        if (program_manager == nullptr)
        {
            append_diagnostic(
                diagnostics,
                L"desktop host was not found and Progman is unavailable");
            return false;
        }

        // Ask Explorer to create the wallpaper WorkerW, then retry both
        // the Windows 11 24H2+ child layout and the legacy sibling layout.
        SetLastError(ERROR_SUCCESS);
        DWORD_PTR message_result = 0;
        const LRESULT message_status = SendMessageTimeoutW(
            program_manager,
            0x052C,
            0,
            0,
            SMTO_ABORTIFHUNG,
            1000,
            &message_result);

        if (message_status == 0)
        {
            append_diagnostic(
                diagnostics,
                L"Progman 0x052C request failed with Win32 error " +
                std::to_wstring(GetLastError()));
        }
        else
        {
            append_diagnostic(
                diagnostics,
                L"Progman 0x052C request completed");
        }

        if (find_progman_layered_layout(
            program_manager,
            layout,
            diagnostics))
        {
            return true;
        }

        if (find_legacy_desktop_parent(layout, diagnostics))
        {
            return true;
        }

        append_diagnostic(
            diagnostics,
            L"desktop host was not found in either known window layout");
        return false;
    }
}
