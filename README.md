# DesktopWorld

Windows desktop background renderer using Win32 and Direct3D 11.

## Implemented

- WorkerW desktop surface host spanning the virtual desktop with one SwapChain. Monitor bounds are enumerated; if the selected WorkerW client area does not contain the full virtual desktop, startup fails rather than showing a partial or misplaced surface.
- Explorer recovery using the TaskbarCreated notification and a periodic HWND health check.
- Direct3D 11 fullscreen sky rendering.
- UTC solar position calculation from latitude and longitude.
- An Environment controller that caches the adjacent solar sample pair and updates it only when the sample window or observer location changes.
- Persistent settings at `%APPDATA%/DesktopWorld/settings.ini`.
- Static atmosphere LUTs: Transmittance, Direct Irradiance, Single Rayleigh, Single Mie, Scattering Density, Multiple Scattering, and Indirect Irradiance.
- RGB Rayleigh/Mie atmospheric model with Mie extinction, triangular ozone absorption profile, diffuse ground albedo, and configurable scattering order (2-4).
- Static LUT disk cache at `%LOCALAPPDATA%/DesktopWorld/Cache`, keyed by physical parameters, LUT dimensions, format and algorithm version.
- Two adjacent time samples for the dynamic Sky View LUT.
- Four selectable render paths: A (direct per-pixel lookup of the 3D scattering LUT), B (current-time Sky View LUT), C (pixel-shader temporal blend), and D (compute-shader temporal blend).
- A serial RenderSystem / RenderPass contract with AtmosphereSkyPass as the initial pass.
- Adaptive, fixed-FPS, and on-demand frame modes.
- Render Once, Update Environment, and Update Environment + Render Once debug actions.
- Debug-only Dear ImGui window in Debug builds; Ctrl+Shift+D toggles it.
- Fixed UTC time, latitude/longitude, render method, sample interval, FPS, Rayleigh/Mie/ozone/ground settings, scattering order, and exposure controls.
- Asynchronous D3D11 timestamp profiling for static precompute stages, Sky View generation, blending, and rendering.
- Debug-only linear-HDR comparison: B vs D at 256 x 128 and A vs D on an aspect-preserving analysis grid capped at 1920 x 1080, reporting RGB MAE / RMSE / maximum absolute error.
- Debug-only PFM capture of linear-HDR Sky View radiance at `%LOCALAPPDATA%/DesktopWorld/Captures`.
- DXGI 1.4 process-level Local / Non-local video-memory usage and OS budget reporting in the Performance tab.
- CPU environment-update and render-submission timings.
- Core unit tests for solar position, sample scheduling (including pre-1970 timestamps), environment sample caching, settings round-trip, frame scheduling, and RenderPass ordering.
- HLSL entry-point compilation validation as part of the Windows build (17 entry points).

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

On first launch the application creates `%APPDATA%/DesktopWorld/settings.ini`. It stores observer latitude/longitude, render method, frame mode and FPS, sample interval override, exposure, and physical atmosphere parameters. The file can be edited directly in Release builds.

Static LUT cache files are placed under `%LOCALAPPDATA%/DesktopWorld/Cache`. Cache headers validate the algorithm version, format, dimensions, and physical parameter hash before reuse.

## Atmosphere model status

The current GPU implementation is an RGB, reduced-resolution, reduced-angular-sample multi-scattering prototype inspired by the staged precomputation structure in Bruneton's reference model. It includes Rayleigh and Mie single scattering, iterative higher-order scattering, a triangular ozone density profile, and diffuse ground reflection in the scattering-density calculation.

The current precomputation uses a 4 x 8 angular quadrature, 24 ray steps for scattering integration, and 48 steps for transmittance. The 3D packed scattering LUT is 256 x 128 x 32, with 64 x 16 irradiance. These are performance-oriented starting values, not reference-quality settings.

The pipeline compiles and passes core unit tests. Debug builds can measure B-vs-D and A-vs-D linear-HDR errors and capture PFM Sky View images, but representative measurements have not yet been collected and physical accuracy against Bruneton reference images remains unverified. Do not treat the current result as a physically validated production atmosphere model.

## Remaining work

- Bruneton reference implementation comparison and physical-model error evaluation.
- Collect B-vs-D / A-vs-D measurements over twilight, date, latitude, and output-resolution variations.
- Tune angular quadrature, ray integration steps, LUT resolutions, and scattering order against image error and GPU time.
- Verify DXGI memory reporting on discrete / integrated / hybrid GPU configurations; per-resource accounting of DesktopWorld-owned allocations is still outstanding.
- Validate cache cold/warm startup and corrupted-cache recovery on real D3D11 devices.
- Explorer-restart recovery, mixed-DPI, monitor topology changes, and runtime visual validation on the target desktop.