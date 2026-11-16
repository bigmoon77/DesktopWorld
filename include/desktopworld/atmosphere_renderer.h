#pragma once

#include "desktopworld/types.h"
#include "desktopworld/gpu_profiler.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace desktopworld
{
    class atmosphere_renderer
    {
    public:
        static constexpr unsigned int sky_width = 256;
        static constexpr unsigned int sky_height = 128;
        static constexpr unsigned int irradiance_width = 64;
        static constexpr unsigned int irradiance_height = 16;
        static constexpr unsigned int scattering_width = 256;
        static constexpr unsigned int scattering_height = 128;
        static constexpr unsigned int scattering_depth = 32;

        bool initialize(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            const wchar_t* shader_path);

        void shutdown();

        bool set_atmosphere_parameters(const atmosphere_parameters& parameters);
        void set_exposure(float exposure);
        void update_current_sky(const solar_state& current);
        void update_sky_samples(
            const solar_state& sample_a,
            const solar_state& sample_b,
            const solar_state& current,
            float blend_factor);

        void draw(
            ID3D11RenderTargetView* target,
            unsigned int width,
            unsigned int height,
            const solar_state& current_sun,
            float blend_factor);

        void set_render_method(render_method method);
        render_method get_render_method() const;

        const atmosphere_parameters& get_parameters() const;
        bool is_initialized() const;

        void begin_profile_frame();
        void end_profile_frame();
        bool get_profile_result(gpu_profile_result& result);

    private:
        struct frame_constants
        {
            float resolution[2]{};
            float inverse_resolution[2]{};
            float sun_direction[3]{};
            float exposure = 1.0f;
            float blend_factor = 0.0f;
            std::uint32_t scattering_order = 0;
            float padding[2]{};
        };

        struct atmosphere_constants
        {
            float rayleigh_scattering[3]{};
            float planet_radius_km = 6360.0f;
            float mie_scattering[3]{};
            float atmosphere_height_km = 100.0f;
            float mie_extinction[3]{};
            float rayleigh_scale_height_km = 8.0f;
            float ozone_absorption[3]{};
            float mie_scale_height_km = 1.2f;
            float ground_albedo[3]{};
            float mie_g = 0.8f;
            float solar_intensity = 20.0f;
            float ozone_center_height_km = 25.0f;
            float ozone_half_width_km = 15.0f;
            float sun_angular_radius = 0.004675f;
            float mu_s_min = -0.207912f;
            std::uint32_t max_scattering_order = 4;
            std::uint32_t angular_theta_samples = 4;
            std::uint32_t angular_phi_samples = 8;
            std::uint32_t reserved = 0;
        };

        bool create_shaders(const wchar_t* shader_path);
        bool create_constant_buffers();
        bool create_textures();
        bool create_static_scattering_textures();
        bool create_render_target(

            unsigned int width,
            unsigned int height,
            ID3D11Texture2D** texture,
            ID3D11ShaderResourceView** srv,
            ID3D11UnorderedAccessView** uav);
        bool create_volume_texture(
            ID3D11Texture3D** texture,
            ID3D11ShaderResourceView** srv,
            ID3D11UnorderedAccessView** uav);
        bool compile_shader(
            const wchar_t* path,
            const char* entry_point,
            const char* target,
            ID3DBlob** bytecode);

        void update_constant_buffers(
            const solar_state& sun,
            float blend_factor,
            std::uint32_t scattering_order = 0);
        void generate_static_luts();
        void generate_transmittance();
        void generate_direct_irradiance();
        void generate_single_scattering();
        void generate_multiple_scattering();
        void generate_sky_view(
            ID3D11UnorderedAccessView* target,
            const solar_state& sun);
        void blend_sky_views(float blend_factor);
        void dispatch_3d(ID3D11ComputeShader* shader);
        void clear_compute_bindings();

        gpu_profiler _profiler;
        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> _context;

        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _transmittance_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _sky_view_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _blend_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _direct_irradiance_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _single_scattering_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _scattering_density_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _multiple_scattering_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _indirect_irradiance_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _accumulate_scattering_cs;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _accumulate_irradiance_cs;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> _fullscreen_vs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _direct_ps;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _current_ps;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _pair_ps;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> _final_ps;

        Microsoft::WRL::ComPtr<ID3D11SamplerState> _linear_sampler;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> _clamp_sampler;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> _rasterizer_state;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _frame_cb;
        Microsoft::WRL::ComPtr<ID3D11Buffer> _atmosphere_cb;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _transmittance_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _transmittance_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _transmittance_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture3D> _single_rayleigh_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _single_rayleigh_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _single_rayleigh_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture3D> _single_mie_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _single_mie_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _single_mie_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture3D> _scattering_sum_a_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _scattering_sum_a_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _scattering_sum_a_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture3D> _scattering_sum_b_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _scattering_sum_b_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _scattering_sum_b_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture3D> _scattering_density_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _scattering_density_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _scattering_density_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture3D> _delta_multiple_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _delta_multiple_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _delta_multiple_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _direct_irradiance_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _direct_irradiance_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _direct_irradiance_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _delta_irradiance_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _delta_irradiance_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _delta_irradiance_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _next_irradiance_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _next_irradiance_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _next_irradiance_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _irradiance_sum_a_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _irradiance_sum_a_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _irradiance_sum_a_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _irradiance_sum_b_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _irradiance_sum_b_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _irradiance_sum_b_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _sky_a_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sky_a_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _sky_a_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _sky_b_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sky_b_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _sky_b_uav;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> _sky_current_texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> _sky_current_srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _sky_current_uav;

        atmosphere_parameters _parameters{};
        render_method _render_method = render_method::sky_blend_compute;
        std::uint64_t _static_parameter_hash = 0;
        bool _static_luts_valid = false;
        bool _initialized = false;
    };
}
