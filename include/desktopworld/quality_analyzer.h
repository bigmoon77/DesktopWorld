#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace desktopworld
{
    struct quality_metrics
    {
        double mean_absolute_error = 0.0;
        double root_mean_square_error = 0.0;
        double maximum_absolute_error = 0.0;
        bool valid = false;
    };

    class quality_analyzer
    {
    public:
        bool initialize(
            ID3D11Device* device,
            const wchar_t* shader_path);
        void shutdown();

        bool measure(
            ID3D11ShaderResourceView* reference,
            ID3D11ShaderResourceView* approximation,
            unsigned int width,
            unsigned int height,
            quality_metrics& result);

        bool is_initialized() const;

    private:
        bool create_shader(const wchar_t* shader_path);
        bool create_error_targets(unsigned int width, unsigned int height);

        Microsoft::WRL::ComPtr<ID3D11Device> _device;
        Microsoft::WRL::ComPtr<ID3D11ComputeShader> _error_cs;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _error_texture;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> _error_uav;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> _staging_texture;

        unsigned int _width = 0;
        unsigned int _height = 0;
        bool _initialized = false;
    };
}
