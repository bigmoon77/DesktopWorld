# DesktopWorld

Windows desktop background renderer using Win32 and Direct3D 11.

## Implemented

- WorkerW-based desktop surface for the primary monitor.
- Direct3D 11 fullscreen sky rendering.
- UTC solar position calculation from latitude and longitude.
- Transmittance LUT generation.
- Numerical Rayleigh/Mie single-scattering evaluation.
- Four selectable render paths:
  - A: direct per-pixel scattering evaluation.
  - B: current-time Sky View LUT.
  - C: two Sky View samples blended in the pixel shader.
  - D: two Sky View samples blended into a low-resolution LUT by compute shader.
- Adaptive, fixed-FPS, and on-demand frame modes.
- Render Once, Update Environment, and Update Environment + Render Once debug actions.
- Debug-only Dear ImGui window in Debug builds; Ctrl+Shift+D toggles it.
- Fixed UTC time, latitude/longitude, render method, sample interval, FPS, atmosphere parameters, and exposure controls.
- Asynchronous D3D11 timestamp profiling for precompute, Sky View generation, blending, and rendering.
- HLSL entry-point compilation validation as part of the Windows build.

## Build

Requirements:

- Windows 10 or Windows 11
- Visual Studio 2022 or newer with the Desktop development with C++ workload
- Windows 10/11 SDK
- CMake 3.24 or newer
- Internet access on the first configure to fetch Dear ImGui

~~~powershell
cmake -S . -B out/build -A x64
cmake --build out/build --config Debug
~~~

The Debug configuration includes the ImGui control window. Release omits the debug UI.

## Debug controls

- Frame mode: Adaptive / Fixed / OnDemand.
- Render Once: draw one frame with the currently active environment data.
- Update Environment: generate current environment data without requesting a desktop draw.
- Update Environment + Render Once: enqueue environment generation and draw once.
- Fixed UTC time, latitude, longitude, and solar position inspection.
- Atmosphere parameter editing and explicit base transmittance LUT rebuild.
- Render method A-D selection and automatic or forced 60/30/15 minute sample intervals.
- CPU frame submission time and asynchronous GPU timestamp measurements.

## Current limitations

This is the first implementation milestone, not the final atmospheric model.

The current atmosphere integrator uses a transmittance LUT and numerical Rayleigh/Mie **single scattering**. Multiple scattering and the complete Bruneton precomputation pipeline are not implemented yet. A-D compare the current single-scattering implementation and its Sky View LUT paths.

The desktop host currently targets the primary monitor. WorkerW is an undocumented Explorer implementation detail; Explorer-restart recovery and full multi-monitor handling remain future work.

GPU memory reporting, persistent user settings, image capture, quantitative reference-image error visualization, and a fully validated multiple-scattering implementation remain future work.
