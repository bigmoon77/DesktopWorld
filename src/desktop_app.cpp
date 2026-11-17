#include "desktopworld/desktop_app.h"

#include "desktopworld/atmosphere_renderer.h"
#include "desktopworld/frame_controller.h"
#include "desktopworld/types.h"
#include "desktopworld/settings_store.h"
#include "desktopworld/time_sample_scheduler.h"
#include "desktopworld/render_system.h"
#include "desktopworld/atmosphere_sky_pass.h"
#include "desktopworld/environment_controller.h"
#include "desktopworld/workerw_host.h"
#include "desktopworld/time_provider.h"
#include "desktopworld/monitor_manager.h"

#include <Windows.h>
#include <ShlObj.h>
#include <d3d11.h>
#include <dxgi1_4.h>
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
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace desktopworld
{
    namespace
    {
        constexpr wchar_t desktop_class_name[] = L"DesktopWorldDesktopWindow";
        constexpr wchar_t debug_class_name[] = L"DesktopWorldDebugWindow";
        std::filesystem::path startup_log_path()
        {
            wchar_t local_app_data[MAX_PATH]{};
            const HRESULT result = SHGetFolderPathW(
                nullptr,
                CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                nullptr,
                SHGFP_TYPE_CURRENT,
                local_app_data);
            if (FAILED(result))
            {
                return {};
            }

            return std::filesystem::path(local_app_data) /
                L"DesktopWorld" / L"Logs" / L"startup.log";
        }

        void write_startup_log(const std::wstring& message)
        {
            OutputDebugStringW(message.c_str());
            OutputDebugStringW(L"\n");

            const std::filesystem::path path = startup_log_path();
            if (path.empty())
            {
                return;
            }

            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return;
            }

            std::wofstream file(path, std::ios::out | std::ios::app);
            if (file.is_open())
            {
                file << message << L"\n";
            }
        }
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

            constexpr std::int64_t windows_to_unix_seconds = 11644473600LL;
            const std::int64_t file_time_seconds =
                static_cast<std::int64_t>(value.QuadPart / 10000000ULL);
            return file_time_seconds - windows_to_unix_seconds;
        }

        SYSTEMTIME unix_seconds_to_system_time(const std::int64_t seconds)
        {
            constexpr std::int64_t windows_to_unix_seconds = 11644473600LL;
            constexpr std::int64_t maximum_unix_seconds = 253402300799LL;
            const std::int64_t clamped_seconds = std::clamp(
                seconds,
                -windows_to_unix_seconds,
                maximum_unix_seconds);
            const std::uint64_t ticks =
                static_cast<std::uint64_t>(
                    clamped_seconds + windows_to_unix_seconds) *
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

#ifdef DW_ENABLE_DEBUG_UI
        static std::filesystem::path create_sky_capture_path()
        {
            wchar_t local_app_data[MAX_PATH]{};
            if (FAILED(SHGetFolderPathW(
                nullptr,
                CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                nullptr,
                SHGFP_TYPE_CURRENT,
                local_app_data)))
            {
                return {};
            }

            const std::filesystem::path directory =
                std::filesystem::path(local_app_data) /
                L"DesktopWorld" /
                L"Captures";

            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error)
            {
                return {};
            }

            SYSTEMTIME time{};
            GetLocalTime(&time);

            std::wostringstream filename;
            filename << L"sky_"
                << std::setfill(L'0')
                << std::setw(4) << time.wYear
                << std::setw(2) << time.wMonth
                << std::setw(2) << time.wDay
                << L"_"
                << std::setw(2) << time.wHour
                << std::setw(2) << time.wMinute
                << std::setw(2) << time.wSecond
                << L"_"
                << std::setw(3) << time.wMilliseconds
                << L".pfm";

            return directory / filename.str();
        }
#endif


    struct desktop_app::implementation
    {
        HINSTANCE instance = nullptr;
        std::wstring initialization_error{};
        HWND desktop_window = nullptr;
        HWND debug_window = nullptr;
        HWND worker_window = nullptr;
        HWND host_monitor_window = nullptr;
        workerw_host desktop_host{};
        monitor_manager displays{};
        RECT desktop_virtual_bounds{};
        int desktop_parent_x = 0;
        int desktop_parent_y = 0;
        bool running = true;
        bool host_rebuild_requested = false;
        bool host_rebuilding = false;
        bool shutting_down = false;
        UINT taskbar_created_message = 0;
        static constexpr UINT_PTR host_check_timer_id = 2;
        bool environment_dirty = true;
        bool current_sky_generated = false;
        int sample_interval_override_seconds = 0;
        bool fixed_time_enabled = false;
        bool debug_window_visible = true;
        settings_store settings_storage{};
        app_settings settings{};
        render_system render{};
        time_provider time_source{};

#ifdef DW_ENABLE_DEBUG_UI
        bool pending_time_dirty = false;
#endif

        unsigned int desktop_width = 0;
        unsigned int desktop_height = 0;

        double latitude = 35.6812;
        double longitude = 139.7671;

        SYSTEMTIME fixed_time{};
        SYSTEMTIME current_time{};

        std::int64_t sample_interval = 0;
        float blend_factor = 0.0f;

        solar_state current_sun{};
        environment_controller environment{};

#ifdef DW_ENABLE_DEBUG_UI
        solar_state sample_sun_a{};
        solar_state sample_sun_b{};
        bool quality_measure_requested = false;
        bool sky_capture_requested = false;
        sky_quality_metrics blend_quality_metrics{};
        sky_quality_metrics direct_quality_metrics{};
        std::filesystem::path last_capture_path{};
        std::string diagnostics_message{};
#endif

        atmosphere_parameters pending_parameters{};

        frame_controller frames{};
        performance_stats stats{};
        std::chrono::steady_clock::time_point last_debug_frame{};
        std::chrono::steady_clock::time_point last_fps_measurement{};
        unsigned int fps_frame_count = 0;

        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<IDXGIFactory> dxgi_factory;
#ifdef DW_ENABLE_DEBUG_UI
        Microsoft::WRL::ComPtr<IDXGIAdapter3> dxgi_adapter3;
        std::chrono::steady_clock::time_point last_gpu_memory_update{};
#endif
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

        bool fail_initialization(
            const wchar_t* stage,
            const HRESULT result = S_OK)
        {
            std::wostringstream message;
            message << L"DesktopWorld startup failure: " << stage;
            if (FAILED(result))
            {
                message << L" (HRESULT=0x"
                    << std::hex << std::uppercase
                    << static_cast<unsigned long>(result) << L")";
            }
            initialization_error = message.str();
            write_startup_log(initialization_error);
            return false;
        }

        bool fail_initialization_win32(
            const wchar_t* stage,
            const DWORD error = GetLastError())
        {
            const HRESULT result = HRESULT_FROM_WIN32(
                error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
            return fail_initialization(stage, result);
        }
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

                if (window == app->desktop_window && message == WM_MOUSEACTIVATE)
                {
                    return MA_NOACTIVATE;
                }

#ifdef DW_ENABLE_DEBUG_UI
                if (window == app->host_monitor_window && message == WM_HOTKEY &&
                    wparam == 1)
                {
                    app->debug_window_visible = !app->debug_window_visible;
                    ShowWindow(
                        app->debug_window,
                        app->debug_window_visible ? SW_SHOW : SW_HIDE);
                    return 0;
                }
#endif

                if (window == app->host_monitor_window &&
                    message == WM_TIMER &&
                    wparam == host_check_timer_id)
                {
                    const HWND current_worker =
                        app->desktop_host.find_desktop_parent();

                    if (app->desktop_window == nullptr ||
                        !IsWindow(app->desktop_window) ||
                        !IsWindow(app->worker_window) ||
                        (current_worker != nullptr &&
                         current_worker != app->worker_window))
                    {
                        app->host_rebuild_requested = true;
                    }
                    return 0;
                }

                if (app->taskbar_created_message != 0 &&
                    message == app->taskbar_created_message)
                {
                    app->host_rebuild_requested = true;
                    return 0;
                }

                if (message == WM_DISPLAYCHANGE || message == WM_DPICHANGED)
                {
                    app->host_rebuild_requested = true;
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
                    app->desktop_window = nullptr;
                    if (!app->host_rebuilding && !app->shutting_down)
                    {
                        app->desktop_render_target.Reset();
                        app->desktop_swap_chain.Reset();
                        app->worker_window = nullptr;
                        app->host_rebuild_requested = true;
                    }
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
            environment.set_observer_location(latitude, longitude);
            environment.set_sample_interval_override(
                sample_interval_override_seconds);
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
            if (atmosphere.is_initialized())
            {
                settings.method = atmosphere.get_render_method();
                settings.atmosphere = atmosphere.get_parameters();
            }
            settings.frame_update_mode = frames.mode();
            settings.sample_interval_override_seconds =
                sample_interval_override_seconds;

#ifdef DW_ENABLE_DEBUG_UI
            settings.base_fps = base_fps;
            settings.fixed_fps = fixed_fps;
            settings.twilight_fps = twilight_fps;
#endif
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

        bool update_desktop_parent_bounds()
        {
            if (worker_window == nullptr ||
                !IsWindow(worker_window) ||
                !displays.is_valid())
            {
                return false;
            }

            const RECT virtual_bounds = displays.virtual_bounds();
            POINT corners[2] =
            {
                { virtual_bounds.left, virtual_bounds.top },
                { virtual_bounds.right, virtual_bounds.bottom }
            };

            SetLastError(ERROR_SUCCESS);
            MapWindowPoints(
                HWND_DESKTOP,
                worker_window,
                corners,
                ARRAYSIZE(corners));

            const DWORD mapping_error = GetLastError();
            if (mapping_error != ERROR_SUCCESS)
            {
                return fail_initialization_win32(
                    L"MapWindowPoints (virtual desktop to WorkerW)",
                    mapping_error);
            }

            desktop_parent_x = corners[0].x;
            desktop_parent_y = corners[0].y;

            RECT parent_client{};
            if (!GetClientRect(worker_window, &parent_client))
            {
                return fail_initialization_win32(
                    L"GetClientRect (WorkerW)");
            }

            const int mapped_width = corners[1].x - corners[0].x;
            const int mapped_height = corners[1].y - corners[0].y;

            if (mapped_width <= 0 ||
                mapped_height <= 0 ||
                desktop_parent_x < parent_client.left ||
                desktop_parent_y < parent_client.top ||
                desktop_parent_x + mapped_width > parent_client.right ||
                desktop_parent_y + mapped_height > parent_client.bottom)
            {
                std::wostringstream message;
                message << L"WorkerW client area does not contain the virtual desktop. "
                    << L"VirtualDesktopMapped=("
                    << desktop_parent_x << L"," << desktop_parent_y << L","
                    << mapped_width << L"," << mapped_height << L") "
                    << L"WorkerWClient=("
                    << parent_client.left << L"," << parent_client.top << L","
                    << (parent_client.right - parent_client.left) << L","
                    << (parent_client.bottom - parent_client.top) << L")";
                initialization_error = message.str();
                write_startup_log(initialization_error);
                return false;
            }

            desktop_virtual_bounds = virtual_bounds;
            desktop_width = static_cast<unsigned int>(mapped_width);
            desktop_height = static_cast<unsigned int>(mapped_height);
            return true;
        }

        bool create_desktop_window()
        {
            std::wstring workerw_diagnostics;
            worker_window = desktop_host.find_desktop_parent(
                &workerw_diagnostics);

            if (!workerw_diagnostics.empty())
            {
                write_startup_log(
                    L"Desktop host discovery: " + workerw_diagnostics);
            }

            if (worker_window == nullptr)
            {
                std::wstring failure =
                    L"find_desktop_parent (WorkerW not found)";
                if (!workerw_diagnostics.empty())
                {
                    failure.append(L"\n");
                    failure.append(workerw_diagnostics);
                }
                return fail_initialization(failure.c_str());
            }

            if (!refresh_desktop_bounds())
            {
                return fail_initialization(
                    L"enumerate display monitors");
            }

            if (!update_desktop_parent_bounds())
            {
                if (initialization_error.empty())
                {
                    fail_initialization(
                        L"map virtual desktop bounds into WorkerW");
                }
                return false;
            }

            desktop_window = CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                desktop_class_name,
                L"DesktopWorld Surface",
                WS_CHILD | WS_VISIBLE,
                desktop_parent_x,
                desktop_parent_y,
                static_cast<int>(desktop_width),
                static_cast<int>(desktop_height),
                worker_window,
                nullptr,
                instance,
                this);

            if (desktop_window == nullptr)
            {
                return fail_initialization_win32(
                    L"CreateWindowExW (desktop child window)");
            }

            if (!SetLayeredWindowAttributes(
                desktop_window,
                0,
                255,
                LWA_ALPHA))
            {
                const DWORD error = GetLastError();
                DestroyWindow(desktop_window);
                desktop_window = nullptr;
                return fail_initialization_win32(
                    L"SetLayeredWindowAttributes (desktop child window)",
                    error);
            }

            return true;
        }

        bool create_windows(const int show_command)
        {
            if (!refresh_desktop_bounds())
            {
                return fail_initialization(
                    L"enumerate display monitors");
            }
            GetSystemTime(&fixed_time);
#ifdef DW_ENABLE_DEBUG_UI
            utc_year = fixed_time.wYear;
            utc_month = fixed_time.wMonth;
            utc_day = fixed_time.wDay;
            utc_hour = fixed_time.wHour;
            utc_minute = fixed_time.wMinute;
#endif

            if (!create_desktop_window())
            {
                return false;
            }

            host_monitor_window = CreateWindowExW(
                WS_EX_TOOLWINDOW,
                desktop_class_name,
                L"DesktopWorld Host Monitor",
                WS_POPUP,
                0,
                0,
                0,
                0,
                nullptr,
                nullptr,
                instance,
                this);

            if (host_monitor_window == nullptr)
            {
                return fail_initialization_win32(
                    L"CreateWindowExW (host monitor window)");
            }

            taskbar_created_message =
                RegisterWindowMessageW(L"TaskbarCreated");

            if (SetTimer(
                host_monitor_window,
                host_check_timer_id,
                3000,
                nullptr) == 0)
            {
                return fail_initialization_win32(
                    L"SetTimer (WorkerW health check)");
            }

#ifdef DW_ENABLE_DEBUG_UI
            RegisterHotKey(
                host_monitor_window,
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
                return fail_initialization_win32(
                    L"CreateWindowExW (debug window)");
            }

            ShowWindow(debug_window, show_command);
            UpdateWindow(debug_window);
            debug_window_visible = true;
#else
            (void)show_command;
#endif
            return true;
        }

        bool refresh_desktop_bounds()
        {
            if (!displays.refresh())
            {
                desktop_width = 0;
                desktop_height = 0;
                return false;
            }

            desktop_virtual_bounds = displays.virtual_bounds();
            desktop_width = static_cast<unsigned int>(
                desktop_virtual_bounds.right - desktop_virtual_bounds.left);
            desktop_height = static_cast<unsigned int>(
                desktop_virtual_bounds.bottom - desktop_virtual_bounds.top);

            if (desktop_width == 0 || desktop_height == 0)
            {
                return false;
            }

            if (desktop_window != nullptr && worker_window != nullptr)
            {
                if (!update_desktop_parent_bounds())
                {
                    return false;
                }

                SetWindowPos(
                    desktop_window,
                    nullptr,
                    desktop_parent_x,
                    desktop_parent_y,
                    static_cast<int>(desktop_width),
                    static_cast<int>(desktop_height),
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }

            return true;
        }

        bool create_desktop_swap_chain()
        {
            if (desktop_window == nullptr || device == nullptr || dxgi_factory == nullptr)
            {
                return fail_initialization(
                    L"create desktop SwapChain (missing HWND, device, or DXGI factory)");
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

            HRESULT result = dxgi_factory->CreateSwapChain(
                device.Get(),
                &description,
                desktop_swap_chain.GetAddressOf());
            if (FAILED(result))
            {
                return fail_initialization(
                    L"IDXGIFactory::CreateSwapChain (desktop surface)",
                    result);
            }

            dxgi_factory->MakeWindowAssociation(
                desktop_window,
                DXGI_MWA_NO_ALT_ENTER);

            result = create_render_target_view(
                device.Get(),
                desktop_swap_chain.Get(),
                desktop_render_target.GetAddressOf());
            if (FAILED(result))
            {
                return fail_initialization(
                    L"create desktop SwapChain back-buffer RenderTargetView",
                    result);
            }
            return true;
        }

        void rebuild_desktop_host()
        {
            if (shutting_down || host_rebuilding)
            {
                return;
            }

            host_rebuild_requested = false;
            host_rebuilding = true;

            desktop_render_target.Reset();
            desktop_swap_chain.Reset();

            if (desktop_window != nullptr && IsWindow(desktop_window))
            {
                DestroyWindow(desktop_window);
                desktop_window = nullptr;
            }

            worker_window = nullptr;
            const bool window_created = create_desktop_window();
            const bool swap_chain_created =
                window_created && create_desktop_swap_chain();

            if (!swap_chain_created)
            {
                desktop_render_target.Reset();
                desktop_swap_chain.Reset();

                if (desktop_window != nullptr && IsWindow(desktop_window))
                {
                    DestroyWindow(desktop_window);
                }
                desktop_window = nullptr;
                worker_window = nullptr;

                OutputDebugStringW(
                    L"DesktopWorld: WorkerW host restore failed; will retry.\n");
            }
            else
            {
                render.resize(desktop_width, desktop_height);
                environment_dirty = true;
                frames.request_frame();
                OutputDebugStringW(
                    L"DesktopWorld: WorkerW host restored.\n");
            }

            host_rebuilding = false;
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
                return fail_initialization(
                    L"D3D11CreateDevice (hardware feature level 11_0)",
                    result);
            }

            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;

            result = device.As(&dxgi_device);
            if (FAILED(result))
            {
                return fail_initialization(L"QueryInterface IDXGIDevice", result);
            }
            result = dxgi_device->GetAdapter(adapter.GetAddressOf());
            if (FAILED(result))
            {
                return fail_initialization(L"IDXGIDevice::GetAdapter", result);
            }
            result = adapter->GetParent(
                IID_PPV_ARGS(dxgi_factory.GetAddressOf()));
            if (FAILED(result))
            {
                return fail_initialization(
                    L"IDXGIAdapter::GetParent (IDXGIFactory)",
                    result);
            }

#ifdef DW_ENABLE_DEBUG_UI
            adapter.As(&dxgi_adapter3);
#endif

            if (!create_desktop_swap_chain())
            {
                return false;
            }

#ifdef DW_ENABLE_DEBUG_UI
            DXGI_SWAP_CHAIN_DESC description{};
            description.BufferDesc.RefreshRate.Numerator = 60;
            description.BufferDesc.RefreshRate.Denominator = 1;
            description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Windowed = TRUE;
            description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
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

            if (FAILED(result))
            {
                return fail_initialization(
                    L"IDXGIFactory::CreateSwapChain (debug window)",
                    result);
            }
            result = create_render_target_view(
                device.Get(),
                debug_swap_chain.Get(),
                debug_render_target.GetAddressOf());
            if (FAILED(result))
            {
                return fail_initialization(
                    L"create debug SwapChain RenderTargetView",
                    result);
            }

            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::StyleColorsDark();

            if (!ImGui_ImplWin32_Init(debug_window))
            {
                return fail_initialization(L"ImGui_ImplWin32_Init");
            }
            if (!ImGui_ImplDX11_Init(device.Get(), context.Get()))
            {
                return fail_initialization(L"ImGui_ImplDX11_Init");
            }
#endif

            wchar_t executable_path[MAX_PATH]{};
            const DWORD executable_path_length =
                GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
            if (executable_path_length == 0 ||
                executable_path_length >= MAX_PATH)
            {
                return fail_initialization_win32(
                    L"GetModuleFileNameW (executable path)");
            }
            const std::filesystem::path shader_directory =
                std::filesystem::path(executable_path).parent_path() / L"shaders";

            if (!std::filesystem::exists(shader_directory))
            {
                return fail_initialization(
                    L"shader directory not found beside executable");
            }

            if (!atmosphere.initialize(
                device.Get(), context.Get(), shader_directory.c_str()))
            {
                return fail_initialization(
                    L"atmosphere_renderer::initialize (see startup.log for shader / resource details)");
            }

            atmosphere.begin_profile_frame();
            const bool atmosphere_parameters_valid =
                atmosphere.set_atmosphere_parameters(settings.atmosphere);
            atmosphere.set_exposure(settings.atmosphere.exposure);
            atmosphere.end_profile_frame();

            if (!atmosphere_parameters_valid)
            {
                return fail_initialization(
                    L"atmosphere_renderer::set_atmosphere_parameters");
            }
            pending_parameters = settings.atmosphere;
            atmosphere.set_render_method(settings.method);

            if (!render.add_pass(
                std::make_unique<atmosphere_sky_pass>(atmosphere)))
            {
                return fail_initialization(L"render_system::add_pass");
            }
            if (!render.initialize())
            {
                return fail_initialization(
                    L"render_system::initialize / atmosphere_sky_pass::initialize");
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

            return time_source.current_utc();
        }

        void update_solar_state()
        {
            current_time = get_current_utc();
        }

        void update_environment()
        {
            environment.set_observer_location(latitude, longitude);
            environment.set_sample_interval_override(
                sample_interval_override_seconds);

            const environment_frame environment_state =
                environment.update(current_time);

            current_sun = environment_state.current_sun;
#ifdef DW_ENABLE_DEBUG_UI
            sample_sun_a = environment_state.sample_sun_a;
            sample_sun_b = environment_state.sample_sun_b;
#endif
            sample_interval =
                environment_state.sample_window.interval_seconds;
            blend_factor = environment_state.sample_window.blend_factor;

            frames.set_solar_elevation(current_sun.elevation_degrees);

            if (environment_state.sample_pair_changed || environment_dirty)
            {
                atmosphere.update_sky_samples(
                    environment_state.sample_sun_a,
                    environment_state.sample_sun_b,
                    current_sun,
                    blend_factor);

                current_sky_generated =
                    atmosphere.get_render_method() == render_method::sky_current;
                environment_dirty = false;
            }
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
                        environment.set_observer_location(latitude, longitude);
                        settings.latitude = latitude;
                        environment_dirty = true;
                        frames.request_frame();
                    }
                    if (ImGui::InputDouble("Longitude", &longitude, 0.01, 0.1, "%.5f"))
                    {
                        longitude = std::clamp(longitude, -180.0, 180.0);
                        environment.set_observer_location(latitude, longitude);
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
                        "Mie scattering RGB",
                        &pending_parameters.mie_scattering.x,
                        0.0f,
                        0.02f,
                        "%.6f");
                    ImGui::SliderFloat3(
                        "Mie extinction RGB",
                        &pending_parameters.mie_extinction.x,
                        0.0f,
                        0.03f,
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
                        "Mie anisotropy g",
                        &pending_parameters.mie_g,
                        -0.98f,
                        0.98f);
                    ImGui::SliderFloat3(
                        "Ozone absorption RGB",
                        &pending_parameters.ozone_absorption.x,
                        0.0f,
                        0.005f,
                        "%.6f");
                    ImGui::SliderFloat(
                        "Ozone center height",
                        &pending_parameters.ozone_center_height_km,
                        5.0f,
                        60.0f);
                    ImGui::SliderFloat(
                        "Ozone half width",
                        &pending_parameters.ozone_half_width_km,
                        1.0f,
                        40.0f);
                    ImGui::SliderFloat3(
                        "Ground albedo RGB",
                        &pending_parameters.ground_albedo.x,
                        0.0f,
                        1.0f);
                    int scattering_order =
                        static_cast<int>(pending_parameters.max_scattering_order);
                    if (ImGui::SliderInt(
                        "Maximum scattering order",
                        &scattering_order,
                        2,
                        4))
                    {
                        pending_parameters.max_scattering_order =
                            static_cast<std::uint32_t>(scattering_order);
                    }
                    ImGui::SliderFloat(
                        "Solar intensity",
                        &pending_parameters.solar_intensity,
                        1.0f,
                        50.0f);

                    if (ImGui::Button("Apply Atmosphere and Rebuild LUTs"))
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
                    ImGui::Text(
                        "Scattering order: %u",
                        pending_parameters.max_scattering_order);
                    ImGui::TextWrapped(
                        "Static LUTs use RGB Rayleigh/Mie single scattering and iterative "
                        "multiple scattering with ozone absorption and diffuse ground "
                        "reflection. Angular precomputation currently uses 4 x 8 samples; "
                        "validate image error and startup time before raising quality.");
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
                        environment.set_sample_interval_override(
                            sample_interval_override_seconds);
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

                    ImGui::Separator();
                    if (ImGui::Button("Measure HDR Errors (B vs D / A vs D)"))
                    {
                        quality_measure_requested = true;
                        frames.request_frame();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Capture Linear HDR Sky (PFM)"))
                    {
                        sky_capture_requested = true;
                        frames.request_frame();
                    }

                    ImGui::TextWrapped(
                        "B vs D compares 256 x 128 linear-HDR LUT pixels. "
                        "A vs D compares direct evaluation with the upsampled "
                        "D result on an aspect-preserving grid capped at 1920 x 1080.");
                    ImGui::TextWrapped(
                        "Readback is synchronous and may briefly stall the GPU. "
                        "Both operations run only after an explicit button press.");

                    if (blend_quality_metrics.valid)
                    {
                        ImGui::Text(
                            "B vs D (%u x %u, %llu pixels)",
                            blend_quality_metrics.width,
                            blend_quality_metrics.height,
                            static_cast<unsigned long long>(
                                blend_quality_metrics.pixel_count));
                        ImGui::Text(
                            "  MAE %.8g  RMSE %.8g  Max %.8g",
                            blend_quality_metrics.mean_absolute_error,
                            blend_quality_metrics.root_mean_squared_error,
                            blend_quality_metrics.maximum_absolute_error);
                    }

                    if (direct_quality_metrics.valid)
                    {
                        ImGui::Text(
                            "A vs D (%u x %u, %llu pixels)",
                            direct_quality_metrics.width,
                            direct_quality_metrics.height,
                            static_cast<unsigned long long>(
                                direct_quality_metrics.pixel_count));
                        ImGui::Text(
                            "  MAE %.8g  RMSE %.8g  Max %.8g",
                            direct_quality_metrics.mean_absolute_error,
                            direct_quality_metrics.root_mean_squared_error,
                            direct_quality_metrics.maximum_absolute_error);
                    }

                    if (!diagnostics_message.empty())
                    {
                        ImGui::TextWrapped("%s", diagnostics_message.c_str());
                    }
                    if (!last_capture_path.empty())
                    {
                        const std::u8string capture_path_utf8 =
                            last_capture_path.u8string();
                        ImGui::TextWrapped(
                            "Last PFM: %s",
                            reinterpret_cast<const char*>(
                                capture_path_utf8.c_str()));
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
                    ImGui::Text("GPU Static LUT total: %.3f ms", stats.gpu_precompute_ms);
                    ImGui::Text("  Transmittance: %.3f ms", stats.gpu_transmittance_ms);
                    ImGui::Text("  Direct irradiance: %.3f ms", stats.gpu_direct_irradiance_ms);
                    ImGui::Text("  Single scattering: %.3f ms", stats.gpu_single_scattering_ms);
                    ImGui::Text("  Multiple scattering: %.3f ms", stats.gpu_multiple_scattering_ms);
                    ImGui::Text("GPU Sky View: %.3f ms", stats.gpu_sky_generation_ms);
                    ImGui::Text("GPU blend: %.3f ms", stats.gpu_blend_ms);
                    ImGui::Text("GPU render: %.3f ms", stats.gpu_render_ms);
                    ImGui::Separator();
                    ImGui::Text("Process video memory (DXGI)");
                    if (stats.gpu_local_memory_valid)
                    {
                        ImGui::Text(
                            "  Local: %.1f / %.1f MiB",
                            stats.gpu_local_memory_usage_mb,
                            stats.gpu_local_memory_budget_mb);
                    }
                    else
                    {
                        ImGui::Text("  Local: unavailable");
                    }
                    if (stats.gpu_non_local_memory_valid)
                    {
                        ImGui::Text(
                            "  Non-local: %.1f / %.1f MiB",
                            stats.gpu_non_local_memory_usage_mb,
                            stats.gpu_non_local_memory_budget_mb);
                    }
                    else
                    {
                        ImGui::Text("  Non-local: unavailable");
                    }
                    ImGui::TextWrapped(
                        "DXGI reports current process usage and the OS budget for "
                        "the selected adapter. This includes all GPU resources owned "
                        "by this process, not only DesktopWorld's atmosphere textures.");
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


#ifdef DW_ENABLE_DEBUG_UI
        void execute_pending_diagnostics()
        {
            if (quality_measure_requested)
            {
                quality_measure_requested = false;
                blend_quality_metrics = {};
                direct_quality_metrics = {};
                sky_quality_metrics blend_result{};
                sky_quality_metrics direct_result{};

                if (atmosphere.analyze_quality(
                    sample_sun_a,
                    sample_sun_b,
                    current_sun,
                    blend_factor,
                    desktop_width,
                    desktop_height,
                    blend_result,
                    direct_result))
                {
                    blend_quality_metrics = blend_result;
                    direct_quality_metrics = direct_result;
                    diagnostics_message =
                        "HDR quality measurement completed.";
                }
                else
                {
                    diagnostics_message =
                        "HDR quality measurement failed. Check D3D11 debug output.";
                }
            }

            if (sky_capture_requested)
            {
                sky_capture_requested = false;
                const std::filesystem::path capture_path =
                    create_sky_capture_path();

                if (!capture_path.empty() &&
                    atmosphere.capture_sky_pfm(
                        capture_path,
                        sample_sun_a,
                        sample_sun_b,
                        current_sun,
                        blend_factor))
                {
                    last_capture_path = capture_path;
                    diagnostics_message =
                        "Linear HDR Sky View LUT saved as PFM.";
                }
                else
                {
                    diagnostics_message =
                        "PFM capture failed. Check the output folder and D3D11 state.";
                }
            }
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


#ifdef DW_ENABLE_DEBUG_UI
        void update_gpu_memory_statistics(
            const std::chrono::steady_clock::time_point now)
        {
            if (last_gpu_memory_update.time_since_epoch().count() != 0 &&
                now - last_gpu_memory_update < std::chrono::seconds(1))
            {
                return;
            }

            last_gpu_memory_update = now;
            stats.gpu_local_memory_valid = false;
            stats.gpu_non_local_memory_valid = false;

            if (!dxgi_adapter3)
            {
                return;
            }

            DXGI_QUERY_VIDEO_MEMORY_INFO local_info{};
            if (SUCCEEDED(dxgi_adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                &local_info)))
            {
                constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
                stats.gpu_local_memory_usage_mb =
                    static_cast<float>(
                        static_cast<double>(local_info.CurrentUsage) /
                        bytes_per_megabyte);
                stats.gpu_local_memory_budget_mb =
                    static_cast<float>(
                        static_cast<double>(local_info.Budget) /
                        bytes_per_megabyte);
                stats.gpu_local_memory_valid = true;
            }

            DXGI_QUERY_VIDEO_MEMORY_INFO non_local_info{};
            if (SUCCEEDED(dxgi_adapter3->QueryVideoMemoryInfo(
                0,
                DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,
                &non_local_info)))
            {
                constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
                stats.gpu_non_local_memory_usage_mb =
                    static_cast<float>(
                        static_cast<double>(non_local_info.CurrentUsage) /
                        bytes_per_megabyte);
                stats.gpu_non_local_memory_budget_mb =
                    static_cast<float>(
                        static_cast<double>(non_local_info.Budget) /
                        bytes_per_megabyte);
                stats.gpu_non_local_memory_valid = true;
            }
        }
#endif

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

                if (host_rebuild_requested)
                {
                    rebuild_desktop_host();
                }

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

#ifdef DW_ENABLE_DEBUG_UI
                execute_pending_diagnostics();
#endif

                gpu_profile_result gpu_result{};
                if (atmosphere.get_profile_result(gpu_result) && gpu_result.valid)
                {
                    stats.gpu_frame_ms = gpu_result.frame_ms;
                    stats.gpu_precompute_ms = gpu_result.precompute_ms;
                    stats.gpu_transmittance_ms = gpu_result.transmittance_ms;
                    stats.gpu_direct_irradiance_ms = gpu_result.direct_irradiance_ms;
                    stats.gpu_single_scattering_ms = gpu_result.single_scattering_ms;
                    stats.gpu_multiple_scattering_ms = gpu_result.multiple_scattering_ms;
                    stats.gpu_sky_generation_ms = gpu_result.sky_generation_ms;
                    stats.gpu_blend_ms = gpu_result.blend_ms;
                    stats.gpu_render_ms = gpu_result.render_ms;
                }

#ifdef DW_ENABLE_DEBUG_UI
                update_gpu_memory_statistics(
                    std::chrono::steady_clock::now());
#endif
            }

            return 0;
        }

        void shutdown()
        {
            shutting_down = true;

            if (host_monitor_window != nullptr && IsWindow(host_monitor_window))
            {
                KillTimer(host_monitor_window, host_check_timer_id);
            }

#ifdef DW_ENABLE_DEBUG_UI
            if (ImGui::GetCurrentContext() != nullptr)
            {
                ImGui_ImplDX11_Shutdown();
                ImGui_ImplWin32_Shutdown();
                ImGui::DestroyContext();
            }
#endif
#ifdef DW_ENABLE_DEBUG_UI
            if (host_monitor_window != nullptr && IsWindow(host_monitor_window))
            {
                UnregisterHotKey(host_monitor_window, 1);
            }
#endif
            save_settings();
            render.shutdown();
            atmosphere.shutdown();
#ifdef DW_ENABLE_DEBUG_UI
            dxgi_adapter3.Reset();
#endif
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
            if (host_monitor_window != nullptr && IsWindow(host_monitor_window))
            {
                DestroyWindow(host_monitor_window);
                host_monitor_window = nullptr;
            }
        }
    };

    int desktop_app::run(const HINSTANCE instance, const int show_command)
    {
        SetProcessDpiAwarenessContext(
            DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        implementation app{};
        app.instance = instance;

        write_startup_log(L"DesktopWorld startup attempt.");

        bool initialized = app.load_settings();
        if (!initialized)
        {
            app.fail_initialization(L"load_settings");
        }
        if (initialized)
        {
            initialized = app.register_window_classes();
            if (!initialized && app.initialization_error.empty())
            {
                app.fail_initialization_win32(L"RegisterClassExW");
            }
        }
        if (initialized)
        {
            initialized = app.create_windows(show_command);
            if (!initialized && app.initialization_error.empty())
            {
                app.fail_initialization(L"create_windows");
            }
        }
        if (initialized)
        {
            initialized = app.create_device_and_swap_chains();
            if (!initialized && app.initialization_error.empty())
            {
                app.fail_initialization(L"create_device_and_swap_chains");
            }
        }

        if (!initialized)
        {
            const std::filesystem::path log_path = startup_log_path();
            std::wostringstream message;
            message << L"DesktopWorld failed to initialize.\n\n"
                << app.initialization_error << L"\n\n"
                << L"Startup log: " << log_path.wstring();
            MessageBoxW(
                nullptr,
                message.str().c_str(),
                L"DesktopWorld",
                MB_OK | MB_ICONERROR);
            app.shutdown();
            return 1;
        }

        write_startup_log(L"DesktopWorld initialization completed.");

        app.update_solar_state();
        app.environment_dirty = true;
        app.frames.request_frame();

        const int result = app.execute();
        app.shutdown();

        return result;
    }
}
