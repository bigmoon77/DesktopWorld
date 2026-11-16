#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <filesystem>

namespace
{
    struct shader_entry
    {
        const char* name;
        const char* target;
    };

    constexpr std::array<shader_entry, 8> shader_entries =
    {{
        { "CS_Transmittance", "cs_5_0" },
        { "CS_SkyView", "cs_5_0" },
        { "CS_Blend", "cs_5_0" },
        { "VS_Fullscreen", "vs_5_0" },
        { "PS_Direct", "ps_5_0" },
        { "PS_SkyCurrent", "ps_5_0" },
        { "PS_SkyPair", "ps_5_0" },
        { "PS_SkyFinal", "ps_5_0" }
    }};

    bool compile_entry(
        const std::filesystem::path& shader_path,
        const shader_entry& entry)
    {
        Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
        Microsoft::WRL::ComPtr<ID3DBlob> errors;

        const HRESULT result = D3DCompileFromFile(
            shader_path.c_str(),
            nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE,
            entry.name,
            entry.target,
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            bytecode.GetAddressOf(),
            errors.GetAddressOf());

        if (FAILED(result))
        {
            std::fprintf(stderr, "Shader compile failed: %s (%s)\n",
                entry.name,
                entry.target);

            if (errors)
            {
                std::fwrite(
                    errors->GetBufferPointer(),
                    1,
                    errors->GetBufferSize(),
                    stderr);
            }

            return false;
        }

        std::printf("Shader OK: %s (%s)\n", entry.name, entry.target);
        return true;
    }
}

int main(const int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "Usage: shader_validation <shader-file>\n");
        return 2;
    }

    const std::filesystem::path shader_path =
        std::filesystem::path(argv[1]);

    bool success = true;
    for (const shader_entry& entry : shader_entries)
    {
        success = compile_entry(shader_path, entry) && success;
    }

    return success ? 0 : 1;
}
