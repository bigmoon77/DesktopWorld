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

        bool send_progman_message(
            const HWND program_manager,
            const WPARAM wparam,
            const LPARAM lparam,
            std::wstring* diagnostics)
        {
            DWORD_PTR message_result = 0;
            SetLastError(ERROR_SUCCESS);

            const LRESULT message_status = SendMessageTimeoutW(
                program_manager,
                0x052C,
                wparam,
                lparam,
                SMTO_ABORTIFHUNG,
                1000,
                &message_result);

            const DWORD message_error = GetLastError();

            std::wostringstream message;
            message << L"Progman 0x052C(wParam=0x"
                << std::hex << std::uppercase
                << static_cast<ULONG_PTR>(wparam)
                << L", lParam=0x"
                << static_cast<ULONG_PTR>(lparam)
                << L")";

            if (message_status == 0)
            {
                message << L" failed with Win32 error "
                    << std::dec << message_error;
            }
            else
            {
                message << L" completed; result=0x"
                    << std::hex << std::uppercase
                    << static_cast<ULONG_PTR>(message_result);
            }

            append_diagnostic(diagnostics, message.str());
            return message_status != 0;
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
            return false;
        }

        append_diagnostic(
            diagnostics,
            L"Progman=" + format_window_handle(program_manager));

        SetLastError(ERROR_SUCCESS);
        const LONG_PTR program_manager_style = GetWindowLongPtrW(
            program_manager,
            GWL_EXSTYLE);
        const DWORD style_error = GetLastError();

        const bool raised_desktop =
            (program_manager_style & WS_EX_NOREDIRECTIONBITMAP) != 0;

        std::wostringstream style_message;
        style_message << L"Progman extended style=0x"
            << std::hex << std::uppercase
            << static_cast<ULONG_PTR>(program_manager_style)
            << L", WS_EX_NOREDIRECTIONBITMAP="
            << (raised_desktop ? L"true" : L"false");

        if (program_manager_style == 0 && style_error != ERROR_SUCCESS)
        {
            style_message << L", GetWindowLongPtrW error="
                << std::dec << style_error;
        }

        append_diagnostic(diagnostics, style_message.str());

        if (find_progman_layered_layout(
            program_manager,
            layout,
            diagnostics))
        {
            return true;
        }

        // Older Explorer layouts may already have a top-level WorkerW
        // following the window that owns SHELLDLL_DefView.
        if (find_legacy_desktop_parent(layout, diagnostics))
        {
            return true;
        }

        if (raised_desktop)
        {
            // On the raised desktop, Progman owns a layered ShellView and
            // creates the wallpaper WorkerW only after these parameterized
            // shell messages. The legacy (0, 0) message alone can complete
            // without creating that child.
            send_progman_message(
                program_manager,
                static_cast<WPARAM>(0x0D),
                static_cast<LPARAM>(0),
                diagnostics);

            if (find_progman_layered_layout(
                program_manager,
                layout,
                diagnostics))
            {
                return true;
            }

            send_progman_message(
                program_manager,
                static_cast<WPARAM>(0x0D),
                static_cast<LPARAM>(1),
                diagnostics);

            if (find_progman_layered_layout(
                program_manager,
                layout,
                diagnostics))
            {
                return true;
            }
        }
        else
        {
            // Preserve the legacy creation path on older Explorer layouts.
            send_progman_message(
                program_manager,
                static_cast<WPARAM>(0),
                static_cast<LPARAM>(0),
                diagnostics);

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
        }

        append_diagnostic(
            diagnostics,
            raised_desktop
                ? L"desktop host was not found after raised-desktop activation"
                : L"desktop host was not found in either known window layout");
        return false;
    }
}
