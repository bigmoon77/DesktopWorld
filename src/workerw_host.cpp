#include "desktopworld/workerw_host.h"

namespace desktopworld
{
    HWND workerw_host::find_desktop_parent() const
    {
        HWND program_manager = FindWindowW(L"Progman", nullptr);
        if (program_manager == nullptr)
        {
            return nullptr;
        }

        DWORD_PTR message_result = 0;
        SendMessageTimeoutW(
            program_manager,
            0x052C,
            0,
            0,
            SMTO_NORMAL,
            1000,
            &message_result);

        struct search_context
        {
            HWND worker = nullptr;
        } context;

        EnumWindows(
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

                search->worker = FindWindowExW(
                    nullptr,
                    window,
                    L"WorkerW",
                    nullptr);
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&context));

        return context.worker;
    }
}
