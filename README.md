# DesktopWorld

Windows desktop background renderer using Win32 and Direct3D 11.

## Current implementation

- WorkerW-based desktop surface for the primary monitor.
- Direct3D 11 fullscreen rendering.
- UTC-based solar position calculation from latitude and longitude.
- Transmittance LUT generation.
- Rayleigh/Mie single-scattering sky evaluation.
- Four rendering paths for comparison:
  - A: per-pixel direct single-scattering evaluation.
  - B: current-time Sky View LUT.
  - C: two time samples blended in the pixel shader.
  - D: two time samples blended into a low-resolution LUT with a compute shader.
- Adaptive, fixed-rate, and on-demand desktop frame scheduling.
- Debug-only ImGui control window in Debug builds.
- Asynchronous D3D11 timestamp profiling for base LUT generation, Sky View generation, blending, and rendering.
- Build-time HLSL entry-point validation on Windows CI.
- Ctrl+Shift+D toggles the debug window.

## Build

Requirements:

- Windows 10 or Windows 11
- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK
- CMake 3.24 or newer
- Internet access on the first configure to fetch Dear ImGui

Configure and build:

\`\`\`powershell
cmake -S . -B out/build -A x64
cmake --build out/build --config Debug
\`\`\`

The Debug configuration includes the ImGui control window. The Release configuration omits the debug UI.

## Debug controls

- Frame mode: Adaptive / Fixed / OnDemand
- Render Once: draw one desktop frame using the currently active environment data.
- Update Environment: generate the current environment data without forcing a desktop frame.
- Update Environment + Render Once: enqueue the required GPU work and then draw one frame.
- Fixed UTC time, latitude, longitude, and solar position inspection.
- Atmosphere parameter editing and explicit base transmittance LUT rebuild.
- Render method A-D selection and sample blend inspection.

## Current limitations

This is the first implementation milestone, not the final atmospheric model.

The current atmosphere integrator uses a transmittance LUT and numerical Rayleigh/Mie **single scattering**. Multiple scattering and the complete Bruneton precomputation pipeline are not implemented yet. The four render methods compare the current single-scattering implementation and its Sky View LUT paths.

The desktop host currently targets the primary monitor. WorkerW is an undocumented Explorer implementation detail; Explorer-restart recovery and full multi-monitor handling remain future work.

GPU timestamp profiling, GPU memory reporting, persistent settings, image capture, and quantitative error visualization are not implemented yet. CPU frame submission time is displayed separately.
