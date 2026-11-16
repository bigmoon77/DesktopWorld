#include "desktopworld/render_system.h"

#include <utility>

namespace desktopworld
{
    bool render_system::add_pass(std::unique_ptr<render_pass> pass)
    {
        if (_initialized || pass == nullptr)
        {
            return false;
        }

        _passes.push_back(std::move(pass));
        return true;
    }

    bool render_system::initialize()
    {
        if (_initialized)
        {
            return true;
        }

        for (std::unique_ptr<render_pass>& pass : _passes)
        {
            if (!pass->initialize())
            {
                shutdown();
                return false;
            }
        }

        _initialized = true;
        return true;
    }

    void render_system::resize(
        const unsigned int width,
        const unsigned int height)
    {
        if (!_initialized)
        {
            return;
        }

        for (const std::unique_ptr<render_pass>& pass : _passes)
        {
            pass->resize(width, height);
        }
    }

    bool render_system::execute(const render_context& context)
    {
        if (!_initialized)
        {
            return false;
        }

        for (const std::unique_ptr<render_pass>& pass : _passes)
        {
            if (!pass->execute(context))
            {
                return false;
            }
        }

        return true;
    }

    void render_system::shutdown()
    {
        for (auto iterator = _passes.rbegin(); iterator != _passes.rend(); ++iterator)
        {
            (*iterator)->shutdown();
        }

        _initialized = false;
    }

    bool render_system::is_initialized() const
    {
        return _initialized;
    }

    std::size_t render_system::pass_count() const
    {
        return _passes.size();
    }
}
