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
    TIMEOUTS Text/DocumentTest=30 Text/RasterTest=30
    BIN_DIR UI/DockWorkspaceTest UI/WidgetTest UI/WidgetCompositionTest)
