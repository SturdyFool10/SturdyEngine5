# Sturdy.Core — concrete graphics implementation layer. RHI defines the neutral contract; Core owns
# API backends (Vulkan/WebGPU/D3D12), shader integration, backend discovery, and graphics-adjacent
# OS/display presentation services.
set(_sturdy_core_excluded_source_dirs)
set(_sturdy_core_graphics_deps Sturdy::VMA Sturdy::Volk Sturdy::Vulkan)
set(_sturdy_core_graphics_defines VK_NO_PROTOTYPES)
if(STURDY_OS STREQUAL "Web")
    list(APPEND _sturdy_core_excluded_source_dirs Vulkan D3D12)
    set(_sturdy_core_graphics_deps Sturdy::WebGPU)
    set(_sturdy_core_graphics_defines)
elseif(NOT STURDY_OS STREQUAL "Windows")
    list(APPEND _sturdy_core_excluded_source_dirs D3D12)
endif()

if(NOT STURDY_OS STREQUAL "Windows")
    list(APPEND _sturdy_core_excluded_source_dirs DirectStorage)
endif()
if(NOT STURDY_OS STREQUAL "Linux")
    list(APPEND _sturdy_core_excluded_source_dirs IoUring)
endif()

# Select one GraphicsPlatform OS subtree.
set(_sturdy_core_graphics_platform_dirs
    GraphicsPlatform/Linux
    GraphicsPlatform/MacOS
    GraphicsPlatform/Windows
    GraphicsPlatform/FreeBSD
    GraphicsPlatform/Web)
set(_sturdy_core_graphics_platform_definitions)
if(STURDY_OS STREQUAL "Windows")
    set(_sturdy_core_graphics_platform_active GraphicsPlatform/Windows)
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_WINDOWS=1)
elseif(STURDY_OS STREQUAL "MacOS")
    set(_sturdy_core_graphics_platform_active GraphicsPlatform/MacOS)
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_MACOS=1)
elseif(STURDY_OS STREQUAL "Linux")
    set(_sturdy_core_graphics_platform_active GraphicsPlatform/Linux)
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_LINUX=1)
elseif(STURDY_OS STREQUAL "FreeBSD")
    set(_sturdy_core_graphics_platform_active GraphicsPlatform/FreeBSD)
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_FREEBSD=1)
elseif(STURDY_OS STREQUAL "Web")
    set(_sturdy_core_graphics_platform_active GraphicsPlatform/Web)
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_WEB=1)
else()
    message(FATAL_ERROR "Unsupported Core graphics-platform OS '${STURDY_OS}'.")
endif()
list(REMOVE_ITEM _sturdy_core_graphics_platform_dirs "${_sturdy_core_graphics_platform_active}")
list(APPEND _sturdy_core_excluded_source_dirs ${_sturdy_core_graphics_platform_dirs})

# GraphicsPlatform is organized OS -> architecture. Exclude nonmatching architecture folders.
if(STURDY_ARCH STREQUAL "x86-64")
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_ARCH_X86_64=1)
    list(APPEND _sturdy_core_excluded_source_dirs Arm64 RISCV)
elseif(STURDY_ARCH STREQUAL "Arm64")
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_ARCH_ARM64=1)
    list(APPEND _sturdy_core_excluded_source_dirs x86-64 RISCV)
elseif(STURDY_ARCH STREQUAL "RISCV")
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_ARCH_RISCV64=1)
    list(APPEND _sturdy_core_excluded_source_dirs x86-64 Arm64)
else()
    message(FATAL_ERROR "Unsupported Core graphics-platform arch '${STURDY_ARCH}'.")
endif()

set(_sturdy_core_streaming_io_deps)
set(_sturdy_core_windows_public_deps)
set(_sturdy_core_windows_private_deps)
if(STURDY_OS STREQUAL "Windows")
    list(APPEND _sturdy_core_streaming_io_deps Sturdy::DirectStorage)
    # DirectX headers appear in the explicit Core/D3D12/RHI escape-hatch headers.
    list(APPEND _sturdy_core_windows_public_deps Sturdy::DirectX)
    list(APPEND _sturdy_core_windows_private_deps Sturdy::D3D12MA dcomp)
endif()

sturdy_add_package(Core
    ARCHIVE_TIMEOUT_SECONDS ${STURDY_STATIC_ARCHIVE_TIMEOUT_SECONDS}

    SOURCES
        "${STURDY_LICENSES_EMBED_CPP}"
        "${STURDY_SHADERS_EMBED_CPP}"

    EXCLUDE_SOURCE_DIRS
        ${_sturdy_core_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::RHI
        Sturdy::WindowManager
        Sturdy::GLM
        Sturdy::SDL3
        Sturdy::Slang
        ${_sturdy_core_graphics_deps}
        ${_sturdy_core_windows_public_deps}

    PRIVATE_DEPS
        Sturdy::Tracy
        Sturdy::gdeflate
        ${_sturdy_core_streaming_io_deps}
        ${_sturdy_core_windows_private_deps}

    PRIVATE_DEFINES
        ${_sturdy_core_graphics_defines}

    PUBLIC_DEFINES
        ${_sturdy_core_graphics_platform_definitions}

    DEBUG_PUBLIC_DEPS
)

if(WIN32 AND STURDY_OS STREQUAL "Windows")
    # GraphicsPlatform's composition-present path uses DXGI + DirectComposition + D3D11 + DWM.
    target_link_libraries(Core PRIVATE dxgi dcomp d3d11 dwmapi)
endif()
