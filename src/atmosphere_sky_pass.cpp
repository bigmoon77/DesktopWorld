#include "desktopworld/atmosphere_sky_pass.h"

namespace desktopworld
{
    atmosphere_sky_pass::atmosphere_sky_pass(atmosphere_renderer& renderer)
        : _renderer(&renderer)
    {
    }

    bool atmosphere_sky_pass::initialize()
    {
        return _renderer != nullptr && _renderer->is_initialized();
    }

    void atmosphere_sky_pass::resize(
        const unsigned int,
        const unsigned int)
    {
    }

    bool atmosphere_sky_pass::execute(const render_context& context)
    {
        if (_renderer == nullptr ||
            context.render_target == nullptr ||
            context.width == 0 ||
            context.height == 0)
        {
            return false;
        }

        _renderer->draw(
            context.render_target,
            context.width,
            context.height,
            context.current_sun,
            context.blend_factor);
        return true;
    }

    void atmosphere_sky_pass::shutdown()
    {
        _renderer = nullptr;
    }
}
