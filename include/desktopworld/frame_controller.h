#pragma once

#include "desktopworld/types.h"
#include <chrono>

namespace desktopworld
{
    class frame_controller
    {
    public:
        using clock = std::chrono::steady_clock;

        void set_mode(frame_mode mode);
        void set_fixed_fps(float fps);
        void set_adaptive_fps(float base_fps, float twilight_fps);
        void set_solar_elevation(float elevation_degrees);

        void request_frame();
        bool should_render(clock::time_point now);
        void on_frame_submitted(clock::time_point now);

        frame_mode mode() const;
        float target_fps() const;
        unsigned int wait_timeout_ms(clock::time_point now) const;

    private:
        frame_mode _mode = frame_mode::adaptive;
        float _fixed_fps = 2.0f;
        float _base_fps = 2.0f;
        float _twilight_fps = 15.0f;
        float _target_fps = 2.0f;
        float _solar_elevation = 45.0f;
        bool _pending_frame = true;
        clock::time_point _next_frame = clock::now();
    };
}
