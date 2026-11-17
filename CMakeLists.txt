cmake_minimum_required(VERSION 3.24)
project(DesktopWorld LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DesktopWorld currently supports Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

set(IMGUI_DIR ${imgui_SOURCE_DIR})
add_library(desktopworld_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(desktopworld_imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_compile_definitions(desktopworld_imgui PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(desktopworld WIN32
    src/main.cpp
    src/solar_model.cpp
    src/frame_controller.cpp
    src/time_sample_scheduler.cpp
    src/environment_controller.cpp
    src/workerw_host.cpp
    src/monitor_manager.cpp
    src/time_provider.cpp
    src/settings_store.cpp
    src/render_system.cpp
    src/atmosphere_sky_pass.cpp
    src/atmosphere_renderer.cpp
    src/gpu_profiler.cpp
    src/desktop_app.cpp
)

target_include_directories(desktopworld PRIVATE include)
target_compile_definitions(desktopworld PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    $<$<CONFIG:Debug>:DW_ENABLE_DEBUG_UI>
)

target_compile_options(desktopworld PRIVATE
    /W4
    /permissive-
    /EHsc
    /utf-8
    /Zc:__cplusplus
)

target_link_libraries(desktopworld PRIVATE
    desktopworld_imgui
    d3d11
    dxgi
    d3dcompiler
    user32
    gdi32
    shell32
    shcore
    dwmapi
    imm32
)

# Layered child windows require the process to declare Windows 8+ support.
set(DESKTOPWORLD_APP_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/desktopworld.manifest")
target_link_options(desktopworld PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${DESKTOPWORLD_APP_MANIFEST}"
)

add_custom_command(TARGET desktopworld POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:desktopworld>/shaders
)

enable_testing()

add_executable(desktopworld_core_tests
    tests/core_tests.cpp
    src/solar_model.cpp
    src/frame_controller.cpp
    src/time_sample_scheduler.cpp
    src/environment_controller.cpp
    src/render_system.cpp
    src/settings_store.cpp
)
target_include_directories(desktopworld_core_tests PRIVATE include)
target_compile_definitions(desktopworld_core_tests PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_options(desktopworld_core_tests PRIVATE
    /W4
    /permissive-
    /EHsc
    /utf-8
    /Zc:__cplusplus
)
target_link_libraries(desktopworld_core_tests PRIVATE shell32)
add_test(NAME desktopworld_core_tests COMMAND desktopworld_core_tests)
set_tests_properties(desktopworld_core_tests PROPERTIES TIMEOUT 45)

add_executable(desktopworld_manifest_validation
    tools/manifest_validation.cpp
)
target_compile_definitions(desktopworld_manifest_validation PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_compile_options(desktopworld_manifest_validation PRIVATE
    /W4
    /permissive-
    /EHsc
    /utf-8
    /Zc:__cplusplus
)
add_dependencies(desktopworld_manifest_validation desktopworld)
add_test(
    NAME desktopworld_manifest_validation
    COMMAND desktopworld_manifest_validation "$<TARGET_FILE:desktopworld>"
)

add_executable(shader_validation tools/shader_validation.cpp)
target_compile_features(shader_validation PRIVATE cxx_std_20)
target_compile_definitions(shader_validation PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(shader_validation PRIVATE d3dcompiler)

add_custom_target(validate_shaders ALL
    COMMAND $<TARGET_FILE:shader_validation>
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders/atmosphere.hlsl
    DEPENDS shader_validation
    VERBATIM
)

option(
    DESKTOPWORLD_BUILD_BRUNETON_COMPARATOR
    "Build the optional CPU reference comparison tool"
    OFF
)

if(DESKTOPWORLD_BUILD_BRUNETON_COMPARATOR)
    find_package(Threads REQUIRED)

    FetchContent_Declare(
        bruneton_reference_source
        GIT_REPOSITORY https://github.com/ebruneton/precomputed_atmospheric_scattering.git
        GIT_TAG 34f14e745cff948f4ca3157d1b62a445ffa7286f
        GIT_SUBMODULES ""
        GIT_SHALLOW FALSE
    )
    FetchContent_Declare(
        bruneton_dimensional_types
        GIT_REPOSITORY https://github.com/ebruneton/dimensional_types.git
        GIT_TAG 84e1e53320cd89119d31ec87cfdc2b51825b0c4b
        GIT_SUBMODULES ""
        GIT_SHALLOW FALSE
    )
    FetchContent_Declare(
        bruneton_progress_bar
        GIT_REPOSITORY https://github.com/ebruneton/progress_bar.git
        GIT_TAG b34cdce09e505e36df90bd7e3b621949f5a52c24
        GIT_SUBMODULES ""
        GIT_SHALLOW FALSE
    )

    FetchContent_GetProperties(bruneton_reference_source)
    if(NOT bruneton_reference_source_POPULATED)
        FetchContent_Populate(bruneton_reference_source)
    endif()

    FetchContent_GetProperties(bruneton_dimensional_types)
    if(NOT bruneton_dimensional_types_POPULATED)
        FetchContent_Populate(bruneton_dimensional_types)
    endif()

    FetchContent_GetProperties(bruneton_progress_bar)
    if(NOT bruneton_progress_bar_POPULATED)
        FetchContent_Populate(bruneton_progress_bar)
    endif()

    add_library(bruneton_reference_cpu STATIC
        ${bruneton_reference_source_SOURCE_DIR}/atmosphere/reference/functions.cc
        ${bruneton_reference_source_SOURCE_DIR}/atmosphere/reference/model.cc
        ${bruneton_progress_bar_SOURCE_DIR}/util/progress_bar.cc
    )
    target_include_directories(bruneton_reference_cpu PRIVATE
        ${bruneton_reference_source_SOURCE_DIR}
        ${bruneton_dimensional_types_SOURCE_DIR}
        ${bruneton_progress_bar_SOURCE_DIR}
    )
    target_link_libraries(bruneton_reference_cpu PRIVATE Threads::Threads)

    add_executable(bruneton_reference_compare EXCLUDE_FROM_ALL
        tools/bruneton_reference_compare.cpp
    )
    target_include_directories(bruneton_reference_compare PRIVATE
        ${bruneton_reference_source_SOURCE_DIR}
        ${bruneton_dimensional_types_SOURCE_DIR}
    )
    target_link_libraries(bruneton_reference_compare PRIVATE
        bruneton_reference_cpu
    )
    target_compile_options(bruneton_reference_compare PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
        /Zc:__cplusplus
    )
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT desktopworld)
