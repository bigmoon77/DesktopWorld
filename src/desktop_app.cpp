#include "desktopworld/desktop_app.h"

#include "desktopworld/atmosphere_renderer.h"
#include "desktopworld/frame_controller.h"
#include "desktopworld/solar_model.h"
#include "desktopworld/types.h"
#include "desktopworld/settings_store.h"
#include "desktopworld/time_sample_scheduler.h"
#include "desktopworld/render_system.h"
#include "desktopworld/atmosphere_sky_pass.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#ifdef DW_ENABLE_DEBUG_UI
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace desktopworld
{
    namespace
    {
        constexpr wchar_t desktop_class_name[] = L"DesktopWorldDesktopWindow";
        constexpr wchar_t debug_class_name[] = L"DesktopWorldDebugWindow";
        std::int64_t system_time_to_unix_seconds(const SYSTEMTIME& time)
        {
            FILETIME file_time{};
            if (!SystemTimeToFileTime(&time, &file_time))
            {
                return 0;
            }

            ULARGE_INTEGER value{};
            value.LowPart = file_time.dwLowDateTime;
            value.HighPart = file_time.dwHighDateTime;

            constexpr std::uint64_t windows_to_unix_seconds = 11644473600ULL;
            return static_cast<std::int64_t>(
                value.QuadPart / 10000000ULL - windows_to_unix_seconds);
        }

        SYSTEMTIME unix_seconds_to_system_time(const std::int64_t seconds)
        {
            constexpr std::uint64_t windows_to_unix_seconds = 11644473600ULL;
            const std::uint64_t ticks =
                (static_cast<std::uint64_t>(seconds) + windows_to_unix_seconds) *
                10000000ULL;

            ULARGE_INTEGER value{};
            value.QuadPart = ticks;

            FILETIME file_time{};
            file_time.dwLowDateTime = value.LowPart;
            file_time.dwHighDateTime = value.HighPart;

            SYSTEMTIME result{};
            FileTimeToSystemTime(&file_time, &result);
            return result;
        }

        HWND find_workerw()
        {
            HWND program_manager = FindWindowW(L"Progman", nullptr);
            if (program_manager != nullptr)
            {
                DWORD_PTR result = 0;
                SendMessageTimeoutW(
                    program_manager,
                    0x052C,
                    0,
                    0,
                    SMTO_NORMAL,
                    1000,
                    &result);
            }

            struct search_context
            {
                HWND worker = nullptr;
            } context;

            EnumWindows(
                [](HWND window, LPARAM parameter) -> BOOL
                {
                    search_context* search =
                        reinterpret_cast<search_context*>(parameter);

                    HWND shell_view = FindWindowExW(
                        window,
                        nullptr,
                        L"SHELLDLL_DefView",
                        nullptr);

                    if (shell_view != nullptr)
                    {
                        search->worker = FindWindowExW(
                            nullptr,
                            window,
                            L"WorkerW",
                            nullptr);
                        return FALSE;
                    }

                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&context));

            return context.worker;
        }

        HRESULT create_render_target_view(
            ID3D11Device* device,
            IDXGISwapChain* swap_chain,
            ID3D11RenderTargetView** render_target)
        {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> back_buffer;
            HRESULT result = swap_chain->GetBuffer(
                0,
                IID_PPV_ARGS(back_buffer.GetAddressOf()));

            if (FAILED(result))
            {
                return result;
            }

            return device->CreateRenderTargetView(
                back_buffer.Get(),
                nullptr,
                render_target);
        }
    }

    struct desktop_app::implementation
    {
        HINSTANCE instance = nullptr;
        HWND desktop_window = nullptr;
        HWND debug_window = nullptr;
        HWND worker_window = nullptr;
        bool running = true;
        bool environment_dirty = true;
        bool current_sky_generated = false;
        int sample_interval_override_seconds = 0;
        bool fixed_time_enabled = false;
        bool debug_window_visible = true;
        settings_store settings_storage{};
        app_settings settings{};
        render_system render{};

#ifdef DW_ENABLE_DEBUG_UI
        bool pending_time_dirty = false;
#endif

        unsigned int desktop_width = 0;
        unsigned int desktop_height = 0;

        double latitude = 35.6812;
        double longitude = 139.7671;

        SYSTEMTIME fixed_time{};
        SYSTEMTIME current_time{};

        std::int64_t sample_start_time = -1;
        std::int64_t sample_interval = 0;
        float blend_factor = 0.0f;

        solar_state current_sun{};
        solar_state sample_sun_a{};
        solar_state sample_sun_b{};

        atmosphere_parameters pending_parameters{};

        frame_controller frames{};
        performance_stats stats{};
        std::chrono::steady_clock::time_point last_debug_frame{};
        std::chrono::steady_clock::time_point last_fps_measurement{};
        unsigned int fps_frame_count = 0;

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<IDXGIFactory> dxgi_factory;
        Microsoft::WRL::ComPtr<IDXGISwapChain> desktop_swap_chain;
        Microsoft::WRL::ComPtr<IDXGISwapChain> debug_swap_chain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> desktop_render_target;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debug_render_target;

        atmosphere_renderer atmosphere;

#ifdef DW_ENABLE_DEBUG_UI
        render_method selected_method = render_method::sky_blend_compute;
        frame_mode selected_frame_mode = frame_mode::adaptive;
        int utc_year = 2026;
        int utc_month = 1;
        int utc_day = 1;
        int utc_hour = 12;
        int utc_minute = 0;
        float base_fps = 2.0f;
        float fixed_fps = 2.0f;
        float twilight_fps = 15.0f;
#endif

        static LRESULT CALLBACK window_proc(
            HWND window,
            UINT message,
            WPARAM wparam,
            LPARAM lparam)
        {
            implementation* app =
                reinterpret_cast<implementation*>(
                    GetWindowLongPtrW(window, GWLP_USERDATA));

            if (message == WM_NCCREATE)
            {
                const CREATESTRUCTW* create_info =
                    reinterpret_cast<const CREATESTRUCTW*>(lparam);
                app = static_cast<implementation*>(create_info->lpCreateParams);
                SetWindowLongPtrW(
                    window,
                    GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(app));
            }

#ifdef DW_ENABLE_DEBUG_UI
            if (app != nullptr && window == app->debug_window &&
                ImGui::GetCurrentContext() != nullptr &&
                ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam))
            {
                return 1;
            }
#endif

            if (app != nullptr)
            {
                if (message == WM_SIZE)
                {
                    const unsigned int width = LOWORD(lparam);
                    const unsigned int height = HIWORD(lparam);
                    if (width > 0 && height > 0)
                    {
                        app->resize_window(window, width, height);
                    }
                    return 0;
                }

                if (window == app->desktop_window && message == WM_NCHITTEST)
                {
                    return HTTRANSPARENT;
                }

#ifdef DW_ENABLE_DEBUG_UI
                if (window == app->desktop_window && message == WM_HOTKEY &&
                    wparam == 1)
                {
                    app->debug_window_visible = !app->debug_window_visible;
                    ShowWindow(
                        app->debug_window,
                        app->debug_window_visible ? SW_SHOW : SW_HIDE);
                    return 0;
                }
#endif

                if (message == WM_DISPLAYCHANGE)
                {
                    app->refresh_desktop_bounds();
                    app->frames.request_frame();
                    return 0;
                }

                if (window == app->debug_window && message == WM_CLOSE)
                {
                    ShowWindow(window, SW_HIDE);
                    app->debug_window_visible = false;
                    return 0;
                }

                if (window == app->desktop_window && message == WM_DESTROY)
                {
                    app->running = false;
                    PostQuitMessage(0);
                    return 0;
                }
            }

            return DefWindowProcW(window, message, wparam, lparam);
        }

        bool load_settings()
        {
            if (!settings_storage.load(settings))
            {
                OutputDebugStringW(
                    L"DesktopWorld: settings could not be loaded; using defaults.\n");
                settings = app_settings{};
            }

            latitude = settings.latitude;
            longitude = settings.longitude;
            sample_interval_override_seconds =
                settings.sample_interval_override_seconds;
            pending_parameters = settings.atmosphere;

            frames.set_adaptive_fps(settings.base_fps, settings.twilight_fps);
            frames.set_fixed_fps(settings.fixed_fps);
            frames.set_mode(settings.frame_update_mode);

#ifdef DW_ENABLE_DEBUG_UI
            selected_method = settings.method;
            selected_frame_mode = settings.frame_update_mode;
            base_fps = settings.base_fps;
            fixed_fps = settings.fixed_fps;
            twilight_fps = settings.twilight_fps;
#endif
            return true;
        }

        void save_settings()
        {
            settings.latitude = latitude;
            settings.longitude = longitude;
            settings.method = atmosphere.get_render_method();
            settings.frame_update_mode = frames.mode();
            settings.sample_interval_override_seconds =
                sample_interval_override_seconds;

#ifdef DW_ENABLE_DEBUG_UI
            settings.base_fps = base_fps;
            settings.fixed_fps = fixed_fps;
            settings.twilight_fps = twilight_fps;
#endif
            settings.atmosphere = atmosphere.get_parameters();

            if (!settings_storage.save(settings))
            {
                OutputDebugStringW(
                    L"DesktopWorld: failed to save settings.\n");
            }
        }

        bool register_window_classes()
        {
            WNDCLASSEXW window_class{};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = window_proc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.lpszClassName = desktop_class_name;
            window_class.style = CS_HREDRAW | CS_VREDRAW;

            if (RegisterClassExW(&window_class) == 0 &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                return false;
            }

#ifdef DW_ENABLE_DEBUG_UI
            window_class.lpszClassName = debug_class_name;
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            if (RegisterClassExW(&window_class) == 0 &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                return false;
            }
#endif
            return true;
        }

        bool create_windows(const int show_command)
        {
            worker_window = find_workerw();
            if (worker_window == nullptr)
            {
                return false;
            }

            refresh_desktop_bounds();
            GetSystemTime(&fixed_time);
#ifdef DW_ENABLE_DEBUG_UI
            utc_year = fixed_time.wYear;
            utc_month = fixed_time.wMonth;
            utc_day = fixed_time.wDay;
            utc_hour = fixed_time.wHour;
            utc_minute = fixed_time.wMinute;
#endif

            desktop_window = CreateWindowExW(
                WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                desktop_class_name,
                L"DesktopWorld Surface",
                WS_CHILD | WS_VISIBLE,
                0,
                0,
                static_cast<int>(desktop_width),
                static_cast<int>(desktop_height),
                worker_window,
                nullptr,
                instance,
                this);

            if (desktop_window == nullptr)
            {
                return false;
            }

#ifdef DW_ENABLE_DEBUG_UI
            RegisterHotKey(
                desktop_window,
                1,
                MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT,
                'D');
            debug_window = CreateWindowExW(
                0,
                debug_class_name,
                L"DesktopWorld Debug",
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                620,
                760,
                nullptr,
                nullptr,
                instance,
                this);

            if (debug_window == nullptr)
            {
                return false;
            }

            ShowWindow(debug_window, show_command);
            UpdateWindow(debug_window);
            debug_window_visible = true;
#else
            (void)show_command;
#endif
            return true;
        }

        void refresh_desktop_bounds()
        {
            desktop_width = static_cast<unsigned int>(GetSystemMetrics(SM_CXSCREEN));
            desktop_height = static_cast<unsigned int>(GetSystemMetrics(SM_CYSCREEN));

            if (desktop_window != nullptr)
            {
                SetWindowPos(
                    desktop_window,
                    nullptr,
                    0,
                    0,
                    static_cast<int>(desktop_width),
                    static_cast<int>(desktop_height),
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        bool create_device_and_swap_chains()
        {
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            D3D_FEATURE_LEVEL feature_level{};
            const D3D_FEATURE_LEVEL requested_levels[] =
            {
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0
            };

            HRESULT result = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                requested_levels,
                ARRAYSIZE(requested_levels),
                D3D11_SDK_VERSION,
                device.GetAddressOf(),
                &feature_level,
                context.GetAddressOf());

#ifdef _DEBUG
            if (FAILED(result))
            {
                flags &= ~D3D11_CREATE_DEVICE_DEBUG;
                result = D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_HARDWARE,
                    nullptr,
                    flags,
                    requested_levels + 1,
                    1,
                    D3D11_SDK_VERSION,
                    device.ReleaseAndGetAddressOf(),
                    &feature_level,
                    context.ReleaseAndGetAddressOf());
            }
#endif
            if (FAILED(result))
            {
                result = D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_HARDWARE,
                    nullptr,
                    flags & ~D3D11_CREATE_DEVICE_DEBUG,
                    requested_levels + 1,
                    1,
                    D3D11_SDK_VERSION,
                    device.ReleaseAndGetAddressOf(),
                    &feature_level,
                    context.ReleaseAndGetAddressOf());
            }
            if (FAILED(result))
            {
                return false;
            }

            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;

            result = device.As(&dxgi_device);
            if (FAILED(result) ||
                FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf())) ||
                FAILED(adapter->GetParent(
                    IID_PPV_ARGS(dxgi_factory.GetAddressOf()))))
            {
                return false;
            }

            DXGI_SWAP_CHAIN_DESC description{};
            description.BufferDesc.Width = desktop_width;
            description.BufferDesc.Height = desktop_height;
            description.BufferDesc.RefreshRate.Numerator = 60;
            description.BufferDesc.RefreshRate.Denominator = 1;
            description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.OutputWindow = desktop_window;
            description.Windowed = TRUE;
            description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            result = dxgi_factory->CreateSwapChain(
                device.Get(),
                &description,
                desktop_swap_chain.GetAddressOf());

            if (FAILED(result))
            {
                return false;
            }

            dxgi_factory->MakeWindowAssociation(
                desktop_window,
                DXGI_MWA_NO_ALT_ENTER);

            if (FAILED(create_render_target_view(
                device.Get(),
                desktop_swap_chain.Get(),
                desktop_render_target.GetAddressOf())))
            {
                return false;
            }

#ifdef DW_ENABLE_DEBUG_UI
            RECT debug_rect{};
            GetClientRect(debug_window, &debug_rect);
            description.BufferDesc.Width =
                static_cast<UINT>(debug_rect.right - debug_rect.left);
            description.BufferDesc.Height =
                static_cast<UINT>(debug_rect.bottom - debug_rect.top);
            description.OutputWindow = debug_window;

            result = dxgi_factory->CreateSwapChain(
                device.Get(),
                &description,
                debug_swap_chain.GetAddressOf());

            if (FAILED(result) ||
                FAILED(create_render_target_view(
                    device.Get(),
                    debug_swap_chain.Get(),
                    debug_render_target.GetAddressOf())))
            {
                return false;
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::StyleColorsDark();

            if (!ImGui_ImplWin32_Init(debug_window) ||
                !ImGui_ImplDX11_Init(device.Get(), context.Get()))
            {
                return false;
            }
#endif

            wchar_t executable_path[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, executable_path, MAX_PATH) == 0)
            {
                return false;
            }
            const std::filesystem::path shader_directory =
                std::filesystem::path(executable_path).parent_path() / L"shaders";

            if (!atmosphere.initialize(
                device.Get(), context.Get(), shader_directory.c_str()))
            {
                return false;
            }

            atmosphere.begin_profile_frame();
            const bool atmosphere_parameters_valid =
                atmosphere.set_atmosphere_parameters(settings.atmosphere);
            atmosphere.set_exposure(settings.atmosphere.exposure);
            atmosphere.end_profile_frame();

            if (!atmosphere_parameters_valid)
            {
                return false;
            }
            pending_parameters = settings.atmosphere;
            atmosphere.set_render_method(settings.method);

            if (!render.add_pass(
                std::make_unique<atmosphere_sky_pass>(atmosphere)) ||
                !render.initialize())
            {
                return false;
            }

            render.resize(desktop_width, desktop_height);
            return true;
        }

        void resize_window(
            const HWND window,
            const unsigned int width,
            const unsigned int height)
        {
            if (window == desktop_window && desktop_swap_chain)
            {
                desktop_render_target.Reset();
                if (SUCCEEDED(desktop_swap_chain->ResizeBuffers(
                    0, width, height, DXGI_FORMAT_UNKNOWN, 0)) &&
                    SUCCEEDED(create_render_target_view(
                        device.Get(),
                        desktop_swap_chain.Get(),
                        desktop_render_target.GetAddressOf())))
                {
                    render.resize(width, height);
                }
                frames.request_frame();
            }

#ifdef DW_ENABLE_DEBUG_UI
            if (window == debug_window && debug_swap_chain)
            {
                debug_render_target.Reset();
                if (SUCCEEDED(debug_swap_chain->ResizeBuffers(
                    0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
                {
                    create_render_target_view(
                        device.Get(),
                        debug_swap_chain.Get(),
                        debug_render_target.GetAddressOf());
                }
            }
#endif
        }

        SYSTEMTIME get_current_utc() const
        {
            if (fixed_time_enabled)
            {
                return fixed_time;
            }

            SYSTEMTIME result{};
            GetSystemTime(&result);
            return result;
        }

        void update_solar_state()
        {
            current_time = get_current_utc();
            current_sun = solar_model::calculate(
                current_time,
                latitude,
                longitude);

            frames.set_solar_elevation(current_sun.elevation_degrees);
        }

        void update_environment()
        {
            const std::int64_t current_epoch =
                system_time_to_unix_seconds(current_time);
            const std::int64_t interval = calculate_sample_interval_seconds(
                current_sun.elevation_degrees,
                sample_interval_override_seconds);
            const time_sample_window sample_window =
                calculate_time_sample_window(current_epoch, interval);

            const bool sample_pair_changed =
                sample_window.start_utc_seconds != sample_start_time ||
                sample_window.interval_seconds != sample_interval;

            if (sample_pair_changed || environment_dirty)
            {
                sample_start_time = sample_window.start_utc_seconds;
                sample_interval = sample_window.interval_seconds;

                const SYSTEMTIME time_a =
                    unix_seconds_to_system_time(sample_window.start_utc_seconds);
                const SYSTEMTIME time_b =
                    unix_seconds_to_system_time(sample_window.end_utc_seconds);

                sample_sun_a = solar_model::calculate(time_a, latitude, longitude);
                sample_sun_b = solar_model::calculate(time_b, latitude, longitude);

                atmosphere.update_sky_samples(
                    sample_sun_a,
                    sample_sun_b,
                    current_sun,
                    sample_window.blend_factor);
                current_sky_generated =
                    atmosphere.get_render_method() == render_method::sky_current;
                environment_dirty = false;
            }

            blend_factor = sample_window.blend_factor;
        }

        void render_desktop()
        {
            if (!desktop_render_target || !desktop_swap_chain)
            {
                return;
            }

            const float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            context->ClearRenderTargetView(
                desktop_render_target.Get(),
                clear_color);

            if (atmosphere.get_render_method() == render_method::sky_current &&
                !current_sky_generated)
            {
                atmosphere.update_current_sky(current_sun);
            }

            render_context frame_context{};
            frame_context.device = device.Get();
            frame_context.immediate_context = context.Get();
            frame_context.render_target = desktop_render_target.Get();
            frame_context.width = desktop_width;
            frame_context.height = desktop_height;
            frame_context.current_sun = current_sun;
            frame_context.blend_factor = blend_factor;

            if (!render.execute(frame_context))
            {
                OutputDebugStringW(
                    L"DesktopWorld: a RenderPass failed to execute.\n");
            }

            desktop_swap_chain->Present(0, 0);
            current_sky_generated = false;
        }

#ifdef DW_ENABLE_DEBUG_UI
        void apply_debug_frame_controls()
        {
            const int selected_method_index = static_cast<int>(selected_method);
            const render_method requested_method =
                static_cast<render_method>(selected_method_index);

            if (requested_method != atmosphere.get_render_method())
            {
                atmosphere.set_render_method(requested_method);
                environment_dirty = true;
            }

            frames.set_adaptive_fps(base_fps, twilight_fps);
            frames.set_fixed_fps(fixed_fps);
            frames.set_mode(selected_frame_mode);

            settings.method = selected_method;
            settings.frame_update_mode = selected_frame_mode;
            settings.base_fps = base_fps;
            settings.fixed_fps = fixed_fps;
            settings.twilight_fps = twilight_fps;
        }

        void draw_debug_gui()
        {
            if (!debug_window_visible || !debug_swap_chain || !debug_render_target)
            {
                return;
            }

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImGui::SetNextWindowSize(
                ImVec2(580.0f, 700.0f),
                ImGuiCond_FirstUseEver);

            ImGui::Begin("DesktopWorld Debug");

            if (ImGui::BeginTabBar("DebugTabs"))
            {
                if (ImGui::BeginTabItem("Frame"))
                {
                    const char* frame_modes[] = { "Adaptive", "Fixed", "OnDemand" };
                    int frame_mode_index = static_cast<int>(selected_frame_mode);

                    if (ImGui::Combo(
                        "Frame mode",
                        &frame_mode_index,
                        frame_modes,
                        IM_ARRAYSIZE(frame_modes)))
                    {
                        selected_frame_mode =
                            static_cast<frame_mode>(frame_mode_index);
                        frames.set_mode(selected_frame_mode);
                        settings.frame_update_mode = selected_frame_mode;
                    }

                    bool adaptive_fps_changed = false;
                    adaptive_fps_changed |= ImGui::SliderFloat(
                        "Base FPS", &base_fps, 1.0f, 30.0f);
                    adaptive_fps_changed |= ImGui::SliderFloat(
                        "Twilight FPS", &twilight_fps, 1.0f, 30.0f);
                    if (adaptive_fps_changed)
                    {
                        frames.set_adaptive_fps(base_fps, twilight_fps);
                        settings.base_fps = base_fps;
                        settings.twilight_fps = twilight_fps;
                    }

                    if (ImGui::SliderFloat(
                        "Fixed FPS", &fixed_fps, 1.0f, 30.0f))
                    {
                        frames.set_fixed_fps(fixed_fps);
                        settings.fixed_fps = fixed_fps;
                    }

                    ImGui::Text("Target FPS: %.2f", frames.target_fps());

                    if (ImGui::Button("Render Once"))
                    {
                        frames.request_frame();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Update Environment"))
                    {
                        environment_dirty = true;
                    }
                    if (ImGui::Button("Update Environment + Render Once"))
                    {
                        environment_dirty = true;
                        frames.request_frame();
                    }

                    ImGui::Separator();
                    ImGui::Text("Frame mode separates screen rendering from LUT generation.");
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Solar"))
                {
                    if (ImGui::Checkbox("Use fixed UTC time", &fixed_time_enabled))
                    {
                        if (fixed_time_enabled)
                        {
                            GetSystemTime(&fixed_time);
                            utc_year = fixed_time.wYear;
                            utc_month = fixed_time.wMonth;
                            utc_day = fixed_time.wDay;
                            utc_hour = fixed_time.wHour;
                            utc_minute = fixed_time.wMinute;
                        }
                        pending_time_dirty = false;
                        environment_dirty = true;
                        frames.request_frame();
                    }
                    if (fixed_time_enabled)
                    {
                        bool time_changed = false;
                        time_changed |= ImGui::InputInt("Year", &utc_year);
                        time_changed |= ImGui::InputInt("Month", &utc_month);
                        time_changed |= ImGui::InputInt("Day", &utc_day);
                        time_changed |= ImGui::InputInt("Hour", &utc_hour);
                        time_changed |= ImGui::InputInt("Minute", &utc_minute);
                        if (time_changed)
                        {
                            pending_time_dirty = true;
                        }

                        if (ImGui::Button("Apply Time"))
                        {
                            SYSTEMTIME candidate{};
                            candidate.wYear = static_cast<WORD>(std::clamp(utc_year, 1601, 9999));
                            candidate.wMonth = static_cast<WORD>(std::clamp(utc_month, 1, 12));
                            candidate.wDay = static_cast<WORD>(std::clamp(utc_day, 1, 31));
                            candidate.wHour = static_cast<WORD>(std::clamp(utc_hour, 0, 23));
                            candidate.wMinute = static_cast<WORD>(std::clamp(utc_minute, 0, 59));
                            candidate.wSecond = 0;

                            FILETIME validation_time{};
                            if (SystemTimeToFileTime(&candidate, &validation_time))
                            {
                                fixed_time = candidate;
                                pending_time_dirty = false;
                                environment_dirty = true;
                                frames.request_frame();
                            }
                            else
                            {
                                ImGui::TextColored(
                                    ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                    "Invalid UTC date/time.");
                            }
                        }
                        if (pending_time_dirty)
                        {
                            ImGui::SameLine();
                            ImGui::TextDisabled("Pending time edit");
                        }

                        if (ImGui::Button("-15 min"))
                        {
                            const std::int64_t epoch =
                                system_time_to_unix_seconds(fixed_time) - 900;
                            fixed_time = unix_seconds_to_system_time(epoch);
                            utc_year = fixed_time.wYear;
                            utc_month = fixed_time.wMonth;
                            utc_day = fixed_time.wDay;
                            utc_hour = fixed_time.wHour;
                            utc_minute = fixed_time.wMinute;
                            pending_time_dirty = false;
                            environment_dirty = true;
                            frames.request_frame();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("+15 min"))
                        {
                            const std::int64_t epoch =
                                system_time_to_unix_seconds(fixed_time) + 900;
                            fixed_time = unix_seconds_to_system_time(epoch);
                            utc_year = fixed_time.wYear;
                            utc_month = fixed_time.wMonth;
                            utc_day = fixed_time.wDay;
                            utc_hour = fixed_time.wHour;
                            utc_minute = fixed_time.wMinute;
                            pending_time_dirty = false;
                            environment_dirty = true;
                            frames.request_frame();
                        }
                    }
                    else
                    {
                        GetSystemTime(&fixed_time);
                        utc_year = fixed_time.wYear;
                        utc_month = fixed_time.wMonth;
                        utc_day = fixed_time.wDay;
                        utc_hour = fixed_time.wHour;
                        utc_minute = fixed_time.wMinute;
                    }

                    if (ImGui::InputDouble("Latitude", &latitude, 0.01, 0.1, "%.5f"))
                    {
                        latitude = std::clamp(latitude, -90.0, 90.0);
                        settings.latitude = latitude;
                        environment_dirty = true;
                        frames.request_frame();
                    }
                    if (ImGui::InputDouble("Longitude", &longitude, 0.01, 0.1, "%.5f"))
                    {
                        longitude = std::clamp(longitude, -180.0, 180.0);
                        settings.longitude = longitude;
                        environment_dirty = true;
                        frames.request_frame();
                    }

                    ImGui::Separator();
                    ImGui::Text(
                        "Solar elevation: %.3f deg",
                        current_sun.elevation_degrees);
                    ImGui::Text(
                        "Solar azimuth: %.3f deg",
                        current_sun.azimuth_degrees);
                    ImGui::Text(
                        "Direction: %.4f, %.4f, %.4f",
                        current_sun.direction.x,
                        current_sun.direction.y,
                        current_sun.direction.z);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Atmosphere"))
                {
                    ImGui::Text("Pending parameters");
                    ImGui::SliderFloat3(
                        "Rayleigh RGB",
                        &pending_parameters.rayleigh_scattering.x,
                        0.0f,
                        0.05f,
                        "%.6f");
                    ImGui::SliderFloat3(
                        "Mie RGB",
                        &pending_parameters.mie_scattering.x,
                        0.0f,
                        0.02f,
                        "%.6f");
                    ImGui::SliderFloat(
                        "Rayleigh scale height",
                        &pending_parameters.rayleigh_scale_height_km,
                        1.0f,
                        20.0f);
                    ImGui::SliderFloat(
                        "Mie scale height",
                        &pending_parameters.mie_scale_height_km,
                        0.2f,
                        5.0f);
                    ImGui::SliderFloat(
                        "Mie g",
                        &pending_parameters.mie_g,
                        0.0f,
                        0.98f);
                    ImGui::SliderFloat(
                        "Solar intensity",
                        &pending_parameters.solar_intensity,
                        1.0f,
                        50.0f);

                    if (ImGui::Button("Apply and Rebuild Base LUT"))
                    {
                        atmosphere.set_atmosphere_parameters(pending_parameters);
                        atmosphere.set_exposure(pending_parameters.exposure);
                        settings.atmosphere = pending_parameters;
                        environment_dirty = true;
                        frames.request_frame();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset Parameters"))
                    {
                        pending_parameters = atmosphere_parameters{};
                    }

                    ImGui::Separator();
                    ImGui::SliderFloat(
                        "Exposure",
                        &pending_parameters.exposure,
                        0.1f,
                        4.0f);
                    if (ImGui::Button("Apply Exposure"))
                    {
                        atmosphere.set_exposure(pending_parameters.exposure);
                        settings.atmosphere.exposure = pending_parameters.exposure;
                        frames.request_frame();
                    }
                    ImGui::TextWrapped(
                        "This first implementation uses a Rayleigh/Mie single-scattering "
                        "integrator with a transmittance LUT. Multiple scattering is not "
                        "implemented yet.");
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Render"))
                {
                    const char* render_methods[] =
                    {
                        "A: Direct evaluation",
                        "B: Current Sky View LUT",
                        "C: Pixel shader blend",
                        "D: Compute shader blend"
                    };

                    int method_index = static_cast<int>(selected_method);
                    if (ImGui::Combo(
                        "Method",
                        &method_index,
                        render_methods,
                        IM_ARRAYSIZE(render_methods)))
                    {
                        selected_method = static_cast<render_method>(method_index);
                        apply_debug_frame_controls();
                        environment_dirty = true;
                        frames.request_frame();
                    }

                    ImGui::Text("Sky LUT: %u x %u",
                        atmosphere_renderer::sky_width,
                        atmosphere_renderer::sky_height);
                    const char* interval_options[] =
                    {
                        "Automatic (solar elevation)",
                        "60 minutes",
                        "30 minutes",
                        "15 minutes"
                    };
                    int interval_index = 0;
                    if (sample_interval_override_seconds == 3600)
                    {
                        interval_index = 1;
                    }
                    else if (sample_interval_override_seconds == 1800)
                    {
                        interval_index = 2;
                    }
                    else if (sample_interval_override_seconds == 900)
                    {
                        interval_index = 3;
                    }
                    if (ImGui::Combo(
                        "Sample interval",
                        &interval_index,
                        interval_options,
                        IM_ARRAYSIZE(interval_options)))
                    {
                        constexpr int interval_values[] = { 0, 3600, 1800, 900 };
                        sample_interval_override_seconds = interval_values[interval_index];
                        settings.sample_interval_override_seconds =
                            sample_interval_override_seconds;
                        environment_dirty = true;
                        frames.request_frame();
                    }
                    ImGui::Text(
                        "Active interval: %lld min",
                        static_cast<long long>(sample_interval / 60));
                    ImGui::Text("Blend factor: %.4f", blend_factor);

                    if (ImGui::Button("Update Sample Pair"))
                    {
                        environment_dirty = true;
                    }
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Performance"))
                {
                    ImGui::Text("Target FPS: %.2f", frames.target_fps());
                    ImGui::Text("Measured FPS: %.2f", stats.current_fps);
                    ImGui::Text("CPU update: %.3f ms", stats.cpu_update_ms);
                    ImGui::Text("CPU render submit: %.3f ms", stats.cpu_frame_ms);
                    ImGui::Text("GPU frame: %.3f ms", stats.gpu_frame_ms);
                    ImGui::Separator();
                    ImGui::Text("GPU base LUT: %.3f ms", stats.gpu_precompute_ms);
                    ImGui::Text("GPU Sky View: %.3f ms", stats.gpu_sky_generation_ms);
                    ImGui::Text("GPU blend: %.3f ms", stats.gpu_blend_ms);
                    ImGui::Text("GPU render: %.3f ms", stats.gpu_render_ms);
                    ImGui::TextWrapped(
                        "GPU durations are collected asynchronously with timestamp "
                        "queries. GPU memory accounting is not implemented yet.");
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::End();
            ImGui::Render();

            const float clear_color[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
            context->OMSetRenderTargets(1, debug_render_target.GetAddressOf(), nullptr);
            context->ClearRenderTargetView(debug_render_target.Get(), clear_color);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            context->OMSetRenderTargets(0, nullptr, nullptr);
            debug_swap_chain->Present(0, 0);
        }
#endif

        void update_fps_statistics(const std::chrono::steady_clock::time_point now)
        {
            ++fps_frame_count;

            if (last_fps_measurement.time_since_epoch().count() == 0)
            {
                last_fps_measurement = now;
                return;
            }

            const double elapsed =
                std::chrono::duration<double>(now - last_fps_measurement).count();

            if (elapsed >= 1.0)
            {
                stats.current_fps =
                    static_cast<float>(static_cast<double>(fps_frame_count) / elapsed);
                fps_frame_count = 0;
                last_fps_measurement = now;
            }
        }

        void process_messages()
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    running = false;
                    break;
                }

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        int execute()
        {
            while (running)
            {
                const std::chrono::steady_clock::time_point before_wait =
                    std::chrono::steady_clock::now();
                unsigned int wait_ms = frames.wait_timeout_ms(before_wait);

#ifdef DW_ENABLE_DEBUG_UI
                if (debug_window_visible)
                {
                    wait_ms = std::min(wait_ms, 33U);
                }
#endif

                MsgWaitForMultipleObjectsEx(
                    0,
                    nullptr,
                    wait_ms,
                    QS_ALLINPUT,
                    MWMO_INPUTAVAILABLE);

                process_messages();
                if (!running)
                {
                    break;
                }

                atmosphere.begin_profile_frame();

#ifdef DW_ENABLE_DEBUG_UI
                const std::chrono::steady_clock::time_point debug_now =
                    std::chrono::steady_clock::now();
                if (debug_window_visible &&
                    (last_debug_frame.time_since_epoch().count() == 0 ||
                     debug_now - last_debug_frame >= std::chrono::milliseconds(33)))
                {
                    draw_debug_gui();
                    last_debug_frame = debug_now;
                }
#endif

                const std::chrono::steady_clock::time_point update_start =
                    std::chrono::steady_clock::now();
                update_solar_state();
                update_environment();
                const std::chrono::steady_clock::time_point update_end =
                    std::chrono::steady_clock::now();
                stats.cpu_update_ms = static_cast<float>(
                    std::chrono::duration<double, std::milli>(
                        update_end - update_start).count());

                const std::chrono::steady_clock::time_point now =
                    std::chrono::steady_clock::now();

                if (frames.should_render(now))
                {
                    const std::chrono::steady_clock::time_point frame_start =
                        std::chrono::steady_clock::now();

                    render_desktop();

                    const std::chrono::steady_clock::time_point frame_end =
                        std::chrono::steady_clock::now();
                    stats.cpu_frame_ms = static_cast<float>(
                        std::chrono::duration<double, std::milli>(
                            frame_end - frame_start).count());

                    frames.on_frame_submitted(frame_end);
                    update_fps_statistics(frame_end);
                }

                atmosphere.end_profile_frame();

                gpu_profile_result gpu_result{};
                if (atmosphere.get_profile_result(gpu_result) && gpu_result.valid)
                {
                    stats.gpu_frame_ms = gpu_result.frame_ms;
                    stats.gpu_precompute_ms = gpu_result.precompute_ms;
                    stats.gpu_sky_generation_ms = gpu_result.sky_generation_ms;
                    stats.gpu_blend_ms = gpu_result.blend_ms;
                    stats.gpu_render_ms = gpu_result.render_ms;
                }
            }

            return 0;
        }

        void shutdown()
        {
#ifdef DW_ENABLE_DEBUG_UI
            if (ImGui::GetCurrentContext() != nullptr)
            {
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            }
#endif
#ifdef DW_ENABLE_DEBUG_UI
            if (desktop_window != nullptr && IsWindow(desktop_window))
            {
                UnregisterHotKey(desktop_window, 1);
            }
#endif
            save_settings();
            render.shutdown();
            atmosphere.shutdown();
            debug_render_target.Reset();
            desktop_render_target.Reset();
            debug_swap_chain.Reset();
            desktop_swap_chain.Reset();
            context.Reset();
            device.Reset();

            if (desktop_window != nullptr && IsWindow(desktop_window))
            {
                DestroyWindow(desktop_window);
                desktop_window = nullptr;
            }
            if (debug_window != nullptr && IsWindow(debug_window))
            {
                DestroyWindow(debug_window);
                debug_window = nullptr;
            }
        }
    };

    int desktop_app::run(const HINSTANCE instance, const int show_command)
    {
        SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        implementation app{};
        app.instance = instance;

        if (!app.load_settings() ||
            !app.register_window_classes() ||
            !app.create_windows(show_command) ||
            !app.create_device_and_swap_chains())
        {
            MessageBoxW(
                nullptr,
                L"DesktopWorld failed to initialize. Check the debug output for details.",
                L"DesktopWorld",
                MB_OK | MB_ICONERROR);
            app.shutdown();
            return 1;
        }

        app.update_solar_state();
        app.environment_dirty = true;
        app.frames.request_frame();

        const int result = app.execute();
        app.shutdown();

        return result;
    }
}
