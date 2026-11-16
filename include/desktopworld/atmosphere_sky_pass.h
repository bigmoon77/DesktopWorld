#pragma once

#include "desktopworld/atmosphere_renderer.h"
#include "desktopworld/render_pass.h"

namespace desktopworld
{
    class atmosphere_sky_pass final : public render_pass
    {
    public:
        explicit atmosphere_sky_pass(atmosphere_renderer& renderer);

        bool initialize() override;
        void resize(unsigned int width, unsigned int height) override;
        bool execute(const render_context& context) override;
        void shutdown() override;

    private:
        atmosphere_renderer* _renderer = nullptr;
    };
}
