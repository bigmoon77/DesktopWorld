# DesktopWorld

Windows desktop background renderer using Win32 and Direct3D 11.

## Implemented

- WorkerW desktop surface host for the primary monitor. If a valid WorkerW host is not found, startup fails rather than drawing into a different shell window.
- Direct3D 11 fullscreen sky rendering.
- UTC solar position calculation from latitude and longitude.
- An Environment controller that caches the adjacent solar sample pair and updates it only when the sample window or observer location changes.
- Persistent settings at `%APPDATA%/DesktopWorld/settings.ini`.
- Transmittance LUT generation.
- Numerical Rayleigh/Mie single-scattering evaluation.
- Four selectable render paths: A (direct per-pixel scattering), B (current-time Sky View LUT), C (pixel-shader blend), and D (compute-shader blend).
- A serial RenderSystem / RenderPass contract with AtmosphereSkyPass as the initial pass.
- Adaptive, fixed-FPS, and on-demand frame modes.
- Render Once, Update Environment, and Update Environment + Render Once debug actions.
- Debug-only Dear ImGui window in Debug builds; Ctrl+Shift+D toggles it.
- Fixed UTC time, latitude/longitude, render method, sample interval, FPS, atmosphere parameters, and exposure controls.
- Asynchronous D3D11 timestamp profiling for precompute, Sky View generation, blending, and rendering.
- CPU environment-update and render-submission timings.
- Core unit tests for solar position, sample scheduling (including pre-1970 timestamps), environment sample caching, settings round-trip, frame scheduling, and RenderPass ordering.
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
ctest --test-dir out/build -C Debug --output-on-failure
~~~

The Debug configuration includes the ImGui control window. Release omits the debug UI.

## Settings

On first launch the application creates `%APPDATA%/DesktopWorld/settings.ini`. It stores observer latitude/longitude, render method, frame mode and FPS, sample interval override, exposure, and current single-scattering parameters. The file can be edited directly in Release builds.

## Current limitations

This is the initial implementation phase, not the final atmospheric model.

The current atmosphere integrator uses a transmittance LUT and numerical Rayleigh/Mie **single scattering**. Multiple scattering, ground reflection, ozone absorption, and the complete Bruneton precomputation pipeline are not implemented yet.

The desktop host currently targets the primary monitor. Explorer-restart recovery, full multi-monitor support, and runtime visual validation on the user's desktop remain future work.

Static atmosphere LUT disk caching, GPU memory reporting, image capture, quantitative B-vs-D radiance error visualization, and reference-image validation remain future work.