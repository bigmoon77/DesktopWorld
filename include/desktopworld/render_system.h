#pragma once

#include "desktopworld/render_pass.h"

#include <memory>
#include <vector>

namespace desktopworld
{
    class render_system
    {
    public:
        bool add_pass(std::unique_ptr<render_pass> pass);
        bool initialize();
        void resize(unsigned int width, unsigned int height);
        bool execute(const render_context& context);
        void shutdown();

        bool is_initialized() const;
        std::size_t pass_count() const;

    private:
        std::vector<std::unique_ptr<render_pass>> _passes;
        bool _initialized = false;
    };
}
