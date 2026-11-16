#include "desktopworld/time_provider.h"

namespace desktopworld
{
    SYSTEMTIME time_provider::current_utc() const
    {
        SYSTEMTIME result{};
        GetSystemTime(&result);
        return result;
    }
}
