# Web's only graphics API is WebGPU (see cmake/SturdyDependencies.cmake's sturdy_configure_webgpu())
# — Vulkan/VMA/volk are desktop-only, and Core/Vulkan has nothing to build against on Web, so its
# whole source tree is excluded there rather than compiling against a Sturdy::Vulkan that was
# never created for this platform.
set(_sturdy_core_excluded_source_dirs)
set(_sturdy_core_graphics_deps Sturdy::VMA Sturdy::Volk Sturdy::Vulkan)
set(_sturdy_core_graphics_defines VK_NO_PROTOTYPES)
if(STURDY_OS STREQUAL "Web")
    set(_sturdy_core_excluded_source_dirs Vulkan)
    set(_sturdy_core_graphics_deps Sturdy::WebGPU)
    set(_sturdy_core_graphics_defines)
endif()

# The D3D12 RHI and this Core wrapper both require Windows/DXGI headers. Keep the wrapper out of
# every non-Windows Core source set just as the root CMake keeps the D3D12 package out of those builds.
if(NOT STURDY_OS STREQUAL "Windows")
    list(APPEND _sturdy_core_excluded_source_dirs D3D12)
endif()

# Per-OS subtrees inside Core/src/Core/Vulkan/ for the texture-streaming fast-I/O backends
# (DirectStorage on Windows, io_uring on Linux) — first use of a nested (not whole-package)
# EXCLUDE_SOURCE_DIRS entry in this package; matches as a path segment anywhere under the source
# root regardless of nesting depth (see SturdyPackage.cmake's own matching logic), so a bare
# directory name is sufficient without spelling out the full "Vulkan/DirectStorage" path.
if(NOT STURDY_OS STREQUAL "Windows")
    list(APPEND _sturdy_core_excluded_source_dirs DirectStorage)
endif()
if(NOT STURDY_OS STREQUAL "Linux")
    list(APPEND _sturdy_core_excluded_source_dirs IoUring)
endif()

set(_sturdy_core_streaming_io_deps)
set(_sturdy_core_platform_graphics_deps)
if(STURDY_OS STREQUAL "Windows")
    list(APPEND _sturdy_core_streaming_io_deps Sturdy::DirectStorage)
    list(APPEND _sturdy_core_platform_graphics_deps Sturdy::D3D12)
endif()

# STURDY_LICENSES_EMBED_CPP is generated at configure time by sturdy_generate_license_embeds()
# (see cmake/SturdyLicenses.cmake) — it implements the :Licenses partition declared in
# Core/Licenses.cppm, #embed-ing every dependency's license text directly into the binary.
# STURDY_SHADERS_EMBED_CPP is its sibling from cmake/SturdyShaders.cmake's
# sturdy_generate_shader_embeds() — it implements Core::Slang::embedded_shaders() (see
# Core/src/Core/Slang/EmbeddedShaders.hpp), #embed-ing every *.slang file under Shaders/.
sturdy_add_package(Core
    ARCHIVE_TIMEOUT_SECONDS ${STURDY_STATIC_ARCHIVE_TIMEOUT_SECONDS}

    SOURCES
        "${STURDY_LICENSES_EMBED_CPP}"
        "${STURDY_SHADERS_EMBED_CPP}"

    EXCLUDE_SOURCE_DIRS
        ${_sturdy_core_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::RHI
        Sturdy::Platform
        Sturdy::GLM
        Sturdy::SDL3
        Sturdy::Slang
        ${_sturdy_core_graphics_deps}
        ${_sturdy_core_platform_graphics_deps}

    PRIVATE_DEPS
        Sturdy::Tracy
        Sturdy::gdeflate
        ${_sturdy_core_streaming_io_deps}

    # volk provides dynamically-loaded entry points; VK_NO_PROTOTYPES keeps the Vulkan headers
    # from also declaring statically-linked prototypes. Set now so volk includes are correct as
    # soon as the renderer starts including <volk.h>. Not applicable on Web (no volk/Vulkan there).
    PRIVATE_DEFINES
        ${_sturdy_core_graphics_defines}

    DEBUG_PUBLIC_DEPS
)
