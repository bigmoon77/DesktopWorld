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

        struct legacy_search_context
        {
            HWND worker = nullptr;
            unsigned int shell_view_parent_count = 0;
            unsigned int child_worker_count = 0;
            unsigned int sibling_worker_count = 0;
        };

        HWND find_legacy_desktop_parent(std::wstring* diagnostics)
        {
            legacy_search_context context{};

            SetLastError(ERROR_SUCCESS);
            const BOOL enumeration_succeeded = EnumWindows(
                [](HWND window, const LPARAM parameter) -> BOOL
                {
                    legacy_search_context* search =
                        reinterpret_cast<legacy_search_context*>(parameter);

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

                    const HWND child_worker = FindWindowExW(
                        window,
                        nullptr,
                        L"WorkerW",
                        nullptr);

                    if (child_worker != nullptr)
                    {
                        ++search->child_worker_count;
                        if (search->worker == nullptr)
                        {
                            search->worker = child_worker;
                        }
                    }

                    const HWND sibling_worker = FindWindowExW(
                        nullptr,
                        window,
                        L"WorkerW",
                        nullptr);

                    if (sibling_worker != nullptr)
                    {
                        ++search->sibling_worker_count;
                        if (search->worker == nullptr)
                        {
                            search->worker = sibling_worker;
                        }
                    }

                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&context));

            append_diagnostic(
                diagnostics,
                L"legacy scan: SHELLDLL_DefView parents=" +
                std::to_wstring(context.shell_view_parent_count) +
                L", child WorkerW=" +
                std::to_wstring(context.child_worker_count) +
                L", sibling WorkerW=" +
                std::to_wstring(context.sibling_worker_count));

            if (!enumeration_succeeded)
            {
                append_diagnostic(
                    diagnostics,
                    L"EnumWindows failed with Win32 error " +
                    std::to_wstring(GetLastError()));
            }

            if (context.worker != nullptr)
            {
                append_diagnostic(
                    diagnostics,
                    L"legacy WorkerW=" +
                    format_window_handle(context.worker));
            }

            return context.worker;
        }
    }

    HWND workerw_host::find_desktop_parent(
        std::wstring* diagnostics) const
    {
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

            // Windows 11 24H2 and later may expose the desktop WorkerW
            // as a child of Progman instead of a top-level sibling.
            HWND worker = FindWindowExW(
                program_manager,
                nullptr,
                L"WorkerW",
                nullptr);

            if (worker != nullptr)
            {
                append_diagnostic(
                    diagnostics,
                    L"found Progman child WorkerW=" +
                    format_window_handle(worker));
                return worker;
            }

            append_diagnostic(
                diagnostics,
                L"no direct WorkerW child under Progman");
        }

        // Older Explorer layouts place the wallpaper WorkerW after the
        // top-level window that owns SHELLDLL_DefView.
        HWND worker = find_legacy_desktop_parent(diagnostics);
        if (worker != nullptr)
        {
            return worker;
        }

        if (program_manager == nullptr)
        {
            append_diagnostic(
                diagnostics,
                L"WorkerW was not found and Progman is unavailable");
            return nullptr;
        }

        // Ask Explorer to create the wallpaper host, then inspect both
        // the newer child-window layout and the legacy top-level layout.
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

        worker = FindWindowExW(
            program_manager,
            nullptr,
            L"WorkerW",
            nullptr);

        if (worker != nullptr)
        {
            append_diagnostic(
                diagnostics,
                L"found Progman child WorkerW after request=" +
                format_window_handle(worker));
            return worker;
        }

        append_diagnostic(
            diagnostics,
            L"no direct WorkerW child after request");

        worker = find_legacy_desktop_parent(diagnostics);
        if (worker != nullptr)
        {
            return worker;
        }

        append_diagnostic(
            diagnostics,
            L"WorkerW was not found in either known window layout");
        return nullptr;
    }
}
