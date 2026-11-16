#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace desktopworld
{
    enum class gpu_profile_scope : std::uint32_t
    {
        precompute = 0,
        sky_generation,
        blend,
        render,
        count
    };

    struct gpu_profile_result
    {
        float frame_ms = 0.0f;
        float precompute_ms = 0.0f;
        float sky_generation_ms = 0.0f;
        float blend_ms = 0.0f;
        float render_ms = 0.0f;
        bool valid = false;
    };

    class gpu_profiler
    {
    public:
        bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
        void shutdown();

        void begin_frame();
        void begin_scope(gpu_profile_scope scope);
        void end_scope(gpu_profile_scope scope);
        void end_frame();

        bool poll(gpu_profile_result& result);
        bool is_initialized() const;

    private:
        static constexpr std::size_t frame_count = 4;
        static constexpr std::size_t scope_count =
            static_cast<std::size_t>(gpu_profile_scope::count);

        struct timestamp_pair
        {
            Microsoft::WRL::ComPtr<ID3D11Query> start;
            Microsoft::WRL::ComPtr<ID3D11Query> end;
            bool start_issued = false;
            bool end_issued = false;
        };

        struct frame_queries
        {
            Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
            timestamp_pair frame;
            std::array<timestamp_pair, scope_count> scopes{};
            bool pending = false;
        };

        bool create_timestamp_pair(timestamp_pair& pair);
        bool create_frame_queries(frame_queries& queries);
        bool collect_frame(frame_queries& queries, gpu_profile_result& result) const;
        static float timestamp_delta_ms(
            std::uint64_t begin,
            std::uint64_t end,
            std::uint64_t frequency,
            bool disjoint);

        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context;
        std::array<frame_queries, frame_count> _frames{};
        gpu_profile_result _latest_result{};
        std::size_t _write_index = 0;
        bool _recording = false;
        bool _has_result = false;
        bool _initialized = false;
    };
}
