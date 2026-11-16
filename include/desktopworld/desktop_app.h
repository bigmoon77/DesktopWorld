#pragma once

#include <Windows.h>

namespace desktopworld
{
    class desktop_app
    {
    public:
        int run(HINSTANCE instance, int show_command);

    private:
        struct implementation;
        implementation* _implementation = nullptr;
    };
}
