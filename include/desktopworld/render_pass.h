#pragma once

#include "desktopworld/render_context.h"

namespace desktopworld
{
    class render_pass
    {
    public:
        virtual ~render_pass() = default;

        virtual bool initialize() = 0;
        virtual void resize(unsigned int width, unsigned int height) = 0;
        virtual bool execute(const render_context& context) = 0;
        virtual void shutdown() = 0;
    };
}
