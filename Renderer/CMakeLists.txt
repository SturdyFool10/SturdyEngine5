# Sturdy.Renderer — high-level rendering services built over RHI/Core. Text shaping/rasterization
# and the immediate-mode UI live here because they are rendering facilities rather than independent
# engine layers.
# One Text/Platform/<OS> font backend is compiled; the others are excluded. FreeBSD uses the Linux one
# (the current POSIX fallback).
sturdy_inactive_os_dirs(_sturdy_renderer_platform_font_dirs
    DIRS Text/Platform/Linux Text/Platform/MacOS Text/Platform/Windows Text/Platform/Web)

sturdy_add_package(Renderer
    ENGINE_OPTIMIZED

    SOURCES
        "${STURDY_FONTS_EMBED_CPP}"

    EXCLUDE_SOURCE_DIRS
        ${_sturdy_renderer_platform_font_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::RHI
        Sturdy::Core
        Sturdy::WindowManager
        Sturdy::GLM
        Sturdy::HarfBuzz
        Sturdy::FreeType
        Sturdy::MsdfGen
        Sturdy::StbImage
        Sturdy::clay

    PRIVATE_DEPS
        Sturdy::Tracy
        Sturdy::SheenBidi
        Sturdy::UniBreak
        Sturdy::LunaSvg
)

sturdy_add_tests(Renderer
    TIMEOUTS Text/DocumentTest=30 Text/RasterTest=30 DisplacementGpuParityTest=180
    BIN_DIR UI/DockWorkspaceTest UI/WidgetTest UI/WidgetCompositionTest)

# The same GPU-parity test against Dawn (WGSL). Built only with WebGPU support; it needs the define to
# compile its WebGPU bring-up in, which the glob-registered Vulkan flavour above does not get.
if(STURDY_ENABLE_WEBGPU)
    sturdy_add_test(Renderer tests/DisplacementGpuParityTest.cpp
        NAME RendererDisplacementGpuParityWebGpuTest
        TIMEOUT 300
        DEFINES DISPLACEMENT_PARITY_WEBGPU=1
        ARGS webgpu)
endif()

# The ray-tracing displacement probe (software fallback) against Dawn (WGSL); same reason as above.
if(STURDY_ENABLE_WEBGPU)
    sturdy_add_test(Renderer tests/DisplacementRayTracingGpuTest.cpp
        NAME RendererDisplacementRayTracingGpuWebGpuTest
        TIMEOUT 300
        DEFINES DISPLACEMENT_RT_WEBGPU=1
        ARGS webgpu)
endif()

# The hierarchy-build / traversal-optimization GPU test against Dawn (WGSL); same reason as above.
if(STURDY_ENABLE_WEBGPU)
    sturdy_add_test(Renderer tests/DisplacementHierarchyBuildGpuTest.cpp
        NAME RendererDisplacementHierarchyBuildGpuWebGpuTest
        TIMEOUT 300
        DEFINES DISPLACEMENT_HB_WEBGPU=1
        ARGS webgpu)
endif()
