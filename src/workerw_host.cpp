#include "desktopworld/workerw_host.h"

namespace desktopworld
{
    namespace
    {
        HWND find_existing_desktop_parent()
        {
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

    HWND workerw_host::find_desktop_parent() const
    {
        HWND worker = find_existing_desktop_parent();
        if (worker != nullptr)
        {
            return worker;
        }

        const HWND program_manager = FindWindowW(L"Progman", nullptr);
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

        return find_existing_desktop_parent();
    }
}
