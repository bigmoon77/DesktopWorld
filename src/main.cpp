#include "desktopworld/desktop_app.h"

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int show_command)
{
    desktopworld::desktop_app app;
    return app.run(instance, show_command);
}
