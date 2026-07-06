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

# STURDY_LICENSES_EMBED_CPP is generated at configure time by sturdy_generate_license_embeds()
# (see cmake/SturdyLicenses.cmake) — it implements the :Licenses partition declared in
# Core/Licenses.cppm, #embed-ing every dependency's license text directly into the binary.
sturdy_add_package(Core
    SOURCES
        "${STURDY_LICENSES_EMBED_CPP}"

    EXCLUDE_SOURCE_DIRS
        ${_sturdy_core_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Platform
        Sturdy::GLM
        Sturdy::SDL3
        Sturdy::Slang
        ${_sturdy_core_graphics_deps}

    # volk provides dynamically-loaded entry points; VK_NO_PROTOTYPES keeps the Vulkan headers
    # from also declaring statically-linked prototypes. Set now so volk includes are correct as
    # soon as the renderer starts including <volk.h>. Not applicable on Web (no volk/Vulkan there).
    PRIVATE_DEFINES
        ${_sturdy_core_graphics_defines}

    DEBUG_PUBLIC_DEPS
)
