#include "desktopworld/frame_controller.h"

#include <algorithm>
#include <cmath>

namespace desktopworld
{
    namespace
    {
        float smooth_step(float edge0, float edge1, float value)
        {
            const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }
    }

    void frame_controller::set_mode(const frame_mode mode)
    {
        _mode = mode;
        request_frame();
    }

    void frame_controller::set_fixed_fps(const float fps)
    {
        _fixed_fps = std::clamp(fps, 1.0f, 30.0f);
        if (_mode == frame_mode::fixed)
        {
            _target_fps = _fixed_fps;
        }
        request_frame();
    }

    void frame_controller::set_adaptive_fps(const float base_fps, const float twilight_fps)
    {
        _base_fps = std::clamp(base_fps, 1.0f, 30.0f);
        _twilight_fps = std::clamp(twilight_fps, _base_fps, 30.0f);
        set_solar_elevation(_solar_elevation);
    }

    void frame_controller::set_solar_elevation(const float elevation_degrees)
    {
        _solar_elevation = elevation_degrees;

        const float transition = 1.0f - smooth_step(
            6.0f,
            18.0f,
            std::abs(_solar_elevation));

        const float adaptive_fps =
            _base_fps + (_twilight_fps - _base_fps) * transition;

        if (_mode == frame_mode::adaptive)
        {
            _target_fps = adaptive_fps;
        }
    }

    void frame_controller::request_frame()
    {
        _pending_frame = true;
    }

    bool frame_controller::should_render(const clock::time_point now)
    {
        if (_mode == frame_mode::on_demand)
        {
            return _pending_frame;
        }

        if (_pending_frame || now >= _next_frame)
        {
            return true;
        }

        return false;
    }

    void frame_controller::on_frame_submitted(const clock::time_point now)
    {
        _pending_frame = false;
        const float fps = std::max(_target_fps, 1.0f);
        _next_frame = now + std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(1.0 / fps));
    }

    frame_mode frame_controller::mode() const
    {
        return _mode;
    }

    float frame_controller::target_fps() const
    {
        if (_mode == frame_mode::fixed)
        {
            return _fixed_fps;
        }
        return _target_fps;
    }

    unsigned int frame_controller::wait_timeout_ms(const clock::time_point now) const
    {
        if (_mode == frame_mode::on_demand && !_pending_frame)
        {
            return 0xFFFFFFFFu;
        }

        const auto remaining = _next_frame - now;
        if (remaining <= clock::duration::zero())
        {
            return 0;
        }

        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();

        return static_cast<unsigned int>(std::clamp<std::int64_t>(milliseconds, 1, 100));
    }
}
