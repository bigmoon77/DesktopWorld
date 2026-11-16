cmake_minimum_required(VERSION 3.24)
project(DesktopWorld LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DesktopWorld currently supports Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9b.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR \${imgui_SOURCE_DIR})
add_library(desktopworld_imgui STATIC
    \${IMGUI_DIR}/imgui.cpp
    \${IMGUI_DIR}/imgui_draw.cpp
    \${IMGUI_DIR}/imgui_tables.cpp
    \${IMGUI_DIR}/imgui_widgets.cpp
    \${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    \${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(desktopworld_imgui PUBLIC
    \${IMGUI_DIR}
    \${IMGUI_DIR}/backends
)
target_compile_definitions(desktopworld_imgui PUBLIC UNICODE _UNICODE)

add_executable(desktopworld WIN32
    src/main.cpp
    src/solar_model.cpp
    src/frame_controller.cpp
    src/atmosphere_renderer.cpp
)

target_include_directories(desktopworld PRIVATE include)
target_compile_definitions(desktopworld PRIVATE
    UNICODE
    _UNICODE
    DW_SHADER_DIR=L"\${CMAKE_CURRENT_BINARY_DIR}/shaders"
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
)

add_custom_command(TARGET desktopworld POST_BUILD
    COMMAND \${CMAKE_COMMAND} -E copy_directory
        \${CMAKE_CURRENT_SOURCE_DIR}/shaders
        \${CMAKE_CURRENT_BINARY_DIR}/shaders
)

set_property(DIRECTORY \${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT desktopworld)
