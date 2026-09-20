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

# The WebGPU RHI backend compiles on native only when Dawn was fetched to provide it; on Web the
# same sources compile against the browser's WebGPU through the Emscripten port, which is always
# present. Excluded rather than #if-d out inside each file so a build without Dawn does not pay
# for parsing thousands of lines that would compile to nothing.
if(STURDY_ENABLE_WEBGPU AND NOT STURDY_OS STREQUAL "Web")
    list(APPEND _sturdy_core_graphics_deps Sturdy::WebGPU)
elseif(NOT STURDY_OS STREQUAL "Web")
    list(APPEND _sturdy_core_excluded_source_dirs WebGPU)
endif()

if(NOT STURDY_OS STREQUAL "Windows")
    list(APPEND _sturdy_core_excluded_source_dirs DirectStorage)
endif()
if(NOT STURDY_OS STREQUAL "Linux")
    list(APPEND _sturdy_core_excluded_source_dirs IoUring)
endif()

# Select one GraphicsPlatform OS subtree, then one architecture folder within it.
set(_sturdy_core_graphics_platform_definitions)
# Gates the WebGPU backend's registration in BackendInventory.cpp. Public because Core's headers do
# not expose it, but the definition has to match the sources that were actually compiled in.
if(STURDY_ENABLE_WEBGPU)
    list(APPEND _sturdy_core_graphics_platform_definitions STURDY_ENABLE_WEBGPU=1)
endif()
sturdy_inactive_os_dirs(_sturdy_core_inactive_os_dirs
    DIRS
        GraphicsPlatform/Linux
        GraphicsPlatform/MacOS
        GraphicsPlatform/Windows
        GraphicsPlatform/FreeBSD
        GraphicsPlatform/Web)
string(TOUPPER "${STURDY_OS}" _sturdy_core_os_define)
list(APPEND _sturdy_core_graphics_platform_definitions "STURDY_GRAPHICS_PLATFORM_${_sturdy_core_os_define}=1")

sturdy_inactive_dirs(_sturdy_core_inactive_arch_dirs KEY "${STURDY_ARCH}" DIRS x86-64 Arm64 RISCV)
if(STURDY_ARCH STREQUAL "x86-64")
    set(_sturdy_core_arch_define X86_64)
elseif(STURDY_ARCH STREQUAL "Arm64")
    set(_sturdy_core_arch_define ARM64)
else()
    set(_sturdy_core_arch_define RISCV64)
endif()
list(APPEND _sturdy_core_graphics_platform_definitions "STURDY_GRAPHICS_PLATFORM_ARCH_${_sturdy_core_arch_define}=1")
list(APPEND _sturdy_core_excluded_source_dirs
    ${_sturdy_core_inactive_os_dirs}
    ${_sturdy_core_inactive_arch_dirs})

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
    ENGINE_OPTIMIZED
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
)

if(WIN32 AND STURDY_OS STREQUAL "Windows")
    # GraphicsPlatform's composition-present path uses DXGI + DirectComposition + D3D11 + DWM.
    target_link_libraries(Core PRIVATE dxgi dcomp d3d11 dwmapi)
endif()

sturdy_add_tests(Core EXCLUDE WebGpuResourceTest)
if(STURDY_ENABLE_WEBGPU)
    sturdy_add_test(Core tests/WebGpuResourceTest.cpp TIMEOUT 60)
endif()
