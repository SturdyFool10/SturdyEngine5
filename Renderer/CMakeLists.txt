# Sturdy.Renderer — high-level rendering services built over RHI/Core. Text shaping/rasterization
# and the immediate-mode UI live here because they are rendering facilities rather than independent
# engine layers.
set(_sturdy_renderer_platform_font_dirs
    Text/Platform/Linux
    Text/Platform/MacOS
    Text/Platform/Windows
    Text/Platform/Web)
if(STURDY_OS STREQUAL "Windows")
    set(_sturdy_renderer_platform_font_active Text/Platform/Windows)
elseif(STURDY_OS STREQUAL "MacOS")
    set(_sturdy_renderer_platform_font_active Text/Platform/MacOS)
elseif(STURDY_OS STREQUAL "Web")
    set(_sturdy_renderer_platform_font_active Text/Platform/Web)
else()
    # Linux implementation is also the current POSIX fallback for FreeBSD.
    set(_sturdy_renderer_platform_font_active Text/Platform/Linux)
endif()
list(REMOVE_ITEM _sturdy_renderer_platform_font_dirs "${_sturdy_renderer_platform_font_active}")

sturdy_add_package(Renderer
    EXCLUDE_SOURCE_DIRS
        tests
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

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(RendererRenderGraphTest tests/RenderGraphTest.cpp)
    target_link_libraries(RendererRenderGraphTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererRenderGraphTest)
    add_test(NAME RendererRenderGraphTest COMMAND RendererRenderGraphTest)
    set_tests_properties(RendererRenderGraphTest PROPERTIES TIMEOUT 10)

    add_executable(RendererReflectionBindingTest tests/ReflectionBindingTest.cpp)
    target_link_libraries(RendererReflectionBindingTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererReflectionBindingTest)
    add_test(NAME RendererReflectionBindingTest COMMAND RendererReflectionBindingTest)
    set_tests_properties(RendererReflectionBindingTest PROPERTIES TIMEOUT 10)

    add_executable(RendererTextDocumentTest tests/Text/DocumentTest.cpp)
    target_link_libraries(RendererTextDocumentTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererTextDocumentTest)
    add_test(NAME RendererTextDocumentTest COMMAND RendererTextDocumentTest)
    set_tests_properties(RendererTextDocumentTest PROPERTIES TIMEOUT 30)

    add_executable(RendererTextRasterTest tests/Text/RasterTest.cpp)
    target_link_libraries(RendererTextRasterTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererTextRasterTest)
    add_test(NAME RendererTextRasterTest COMMAND RendererTextRasterTest)
    set_tests_properties(RendererTextRasterTest PROPERTIES TIMEOUT 30)

    add_executable(RendererUIDockWorkspaceTest tests/UI/DockWorkspaceTest.cpp)
    target_link_libraries(RendererUIDockWorkspaceTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererUIDockWorkspaceTest)
    add_test(NAME RendererUIDockWorkspaceTest COMMAND RendererUIDockWorkspaceTest)
    set_tests_properties(RendererUIDockWorkspaceTest PROPERTIES TIMEOUT 10)

    add_executable(RendererUIWidgetTest tests/UI/WidgetTest.cpp)
    target_link_libraries(RendererUIWidgetTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererUIWidgetTest)
    add_test(NAME RendererUIWidgetTest COMMAND RendererUIWidgetTest)
    set_tests_properties(RendererUIWidgetTest PROPERTIES TIMEOUT 10)

    add_executable(RendererUIWidgetCompositionTest tests/UI/WidgetCompositionTest.cpp)
    target_link_libraries(RendererUIWidgetCompositionTest PRIVATE Sturdy::Renderer)
    sturdy_enable_warnings(RendererUIWidgetCompositionTest)
    add_test(NAME RendererUIWidgetCompositionTest COMMAND RendererUIWidgetCompositionTest)
    set_tests_properties(RendererUIWidgetCompositionTest PROPERTIES TIMEOUT 10)

    set_target_properties(
        RendererUIDockWorkspaceTest RendererUIWidgetTest RendererUIWidgetCompositionTest
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
