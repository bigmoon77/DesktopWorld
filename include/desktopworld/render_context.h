#pragma once

#include "desktopworld/types.h"

#include <d3d11.h>

namespace desktopworld
{
    struct render_context
    {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* immediate_context = nullptr;
        ID3D11RenderTargetView* render_target = nullptr;

        unsigned int width = 0;
        unsigned int height = 0;

        solar_state current_sun{};
        float blend_factor = 0.0f;
    };
}
