#include "desktopworld/gpu_profiler.h"

#include <algorithm>

namespace desktopworld
{
    bool gpu_profiler::initialize(
        ID3D11Device* device,
        ID3D11DeviceContext* context)
    {
        if (device == nullptr || context == nullptr)
        {
            return false;
        }

        _device = device;
        _context = context;

        for (frame_queries& queries : _frames)
        {
            if (!create_frame_queries(queries))
            {
                shutdown();
                return false;
            }
        }

        _initialized = true;
        return true;
    }

    void gpu_profiler::shutdown()
    {
        _recording = false;
        _initialized = false;

        for (frame_queries& queries : _frames)
        {
            queries.disjoint.Reset();
            queries.frame.start.Reset();
            queries.frame.end.Reset();
            for (timestamp_pair& pair : queries.scopes)
            {
                pair.start.Reset();
                pair.end.Reset();
            }
            queries.pending = false;
        }

        _context.Reset();
        _device.Reset();
        _latest_result = {};
        _write_index = 0;
        _has_result = false;
    }

    void gpu_profiler::begin_frame()
    {
        if (!_initialized || _recording)
        {
            return;
        }

        frame_queries& queries = _frames[_write_index];
        if (queries.pending)
        {
            gpu_profile_result completed{};
            if (!collect_frame(queries, completed))
            {
                return;
            }

            _latest_result = completed;
            _has_result = true;
            queries.pending = false;
        }

        queries.frame.start_issued = false;
        queries.frame.end_issued = false;
        for (timestamp_pair& pair : queries.scopes)
        {
            pair.start_issued = false;
            pair.end_issued = false;
        }

        _context->Begin(queries.disjoint.Get());
        _context->End(queries.frame.start.Get());
        queries.frame.start_issued = true;
        _recording = true;
    }

    void gpu_profiler::begin_scope(const gpu_profile_scope scope)
    {
        if (!_initialized || !_recording)
        {
            return;
        }

        const std::size_t index = static_cast<std::size_t>(scope);
        if (index >= scope_count)
        {
            return;
        }

        timestamp_pair& pair = _frames[_write_index].scopes[index];
        _context->End(pair.start.Get());
        pair.start_issued = true;
        pair.end_issued = false;
    }

    void gpu_profiler::end_scope(const gpu_profile_scope scope)
    {
        if (!_initialized || !_recording)
        {
            return;
        }

        const std::size_t index = static_cast<std::size_t>(scope);
        if (index >= scope_count)
        {
            return;
        }

        timestamp_pair& pair = _frames[_write_index].scopes[index];
        if (!pair.start_issued)
        {
            return;
        }

        _context->End(pair.end.Get());
        pair.end_issued = true;
    }

    void gpu_profiler::end_frame()
    {
        if (!_initialized || !_recording)
        {
            return;
        }

        frame_queries& queries = _frames[_write_index];
        _context->End(queries.frame.end.Get());
        queries.frame.end_issued = true;
        _context->End(queries.disjoint.Get());

        queries.pending = true;
        _recording = false;
        _write_index = (_write_index + 1) % frame_count;
    }

    bool gpu_profiler::poll(gpu_profile_result& result)
    {
        if (!_initialized)
        {
            return false;
        }

        bool found = false;
        for (std::size_t offset = 0; offset < frame_count; ++offset)
        {
            const std::size_t index =
                (_write_index + frame_count - 1 - offset) % frame_count;

            frame_queries& queries = _frames[index];

            if (!queries.pending)
            {
                continue;
            }

            gpu_profile_result completed{};
            if (collect_frame(queries, completed))
            {
                queries.pending = false;
                _latest_result = completed;
                _has_result = true;
                found = true;
                break;
            }
        }

        if (found || _has_result)
        {
            result = _latest_result;
            return true;
        }

        return false;
    }

    bool gpu_profiler::is_initialized() const
    {
        return _initialized;
    }

    bool gpu_profiler::create_timestamp_pair(timestamp_pair& pair)
    {
        D3D11_QUERY_DESC description{};
        description.Query = D3D11_QUERY_TIMESTAMP;

        return SUCCEEDED(_device->CreateQuery(
            &description,
            pair.start.GetAddressOf())) &&
            SUCCEEDED(_device->CreateQuery(
                &description,
                pair.end.GetAddressOf()));
    }

    bool gpu_profiler::create_frame_queries(frame_queries& queries)
    {
        D3D11_QUERY_DESC disjoint_description{};
        disjoint_description.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

        if (FAILED(_device->CreateQuery(
            &disjoint_description,
            queries.disjoint.GetAddressOf())) ||
            !create_timestamp_pair(queries.frame))
        {
            return false;
        }

        for (timestamp_pair& pair : queries.scopes)
        {
            if (!create_timestamp_pair(pair))
            {
                return false;
            }
        }

        return true;
    }

    bool gpu_profiler::collect_frame(
        frame_queries& queries,
        gpu_profile_result& result) const
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint_data{};
        const HRESULT disjoint_result = _context->GetData(
            queries.disjoint.Get(),
            &disjoint_data,
            sizeof(disjoint_data),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);

        if (disjoint_result != S_OK)
        {
            return false;
        }

        if (!queries.frame.start_issued || !queries.frame.end_issued)
        {
            return false;
        }

        std::uint64_t frame_start = 0;
        std::uint64_t frame_end = 0;

        if (_context->GetData(
                queries.frame.start.Get(),
                &frame_start,
                sizeof(frame_start),
                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            _context->GetData(
                queries.frame.end.Get(),
                &frame_end,
                sizeof(frame_end),
                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        {
            return false;
        }

        result = {};
        result.frame_ms = timestamp_delta_ms(
            frame_start,
            frame_end,
            disjoint_data.Frequency,
            disjoint_data.Disjoint != FALSE);

        const auto read_scope = [&](const gpu_profile_scope scope) -> float
        {
            const timestamp_pair& pair =
                queries.scopes[static_cast<std::size_t>(scope)];

            if (!pair.start_issued || !pair.end_issued)
            {
                return 0.0f;
            }

            std::uint64_t start = 0;
            std::uint64_t end = 0;

            if (_context->GetData(
                    pair.start.Get(),
                    &start,
                    sizeof(start),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                _context->GetData(
                    pair.end.Get(),
                    &end,
                    sizeof(end),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            {
                return 0.0f;
            }

            return timestamp_delta_ms(
                start,
                end,
                disjoint_data.Frequency,
                disjoint_data.Disjoint != FALSE);
        };

        result.precompute_ms = read_scope(gpu_profile_scope::precompute);
        result.transmittance_ms = read_scope(gpu_profile_scope::transmittance);
        result.direct_irradiance_ms = read_scope(gpu_profile_scope::direct_irradiance);
        result.single_scattering_ms = read_scope(gpu_profile_scope::single_scattering);
        result.multiple_scattering_ms = read_scope(gpu_profile_scope::multiple_scattering);
        result.sky_generation_ms = read_scope(gpu_profile_scope::sky_generation);
        result.blend_ms = read_scope(gpu_profile_scope::blend);
        result.render_ms = read_scope(gpu_profile_scope::render);
        result.valid = !disjoint_data.Disjoint && disjoint_data.Frequency != 0;
        return true;
    }

    float gpu_profiler::timestamp_delta_ms(
        const std::uint64_t begin,
        const std::uint64_t end,
        const std::uint64_t frequency,
        const bool disjoint)
    {
        if (disjoint || frequency == 0 || end < begin)
        {
            return 0.0f;
        }

        const double ticks = static_cast<double>(end - begin);
        const double milliseconds =
            ticks * 1000.0 / static_cast<double>(frequency);

        return static_cast<float>(milliseconds);
    }
}
