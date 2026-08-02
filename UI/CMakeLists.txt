# Sturdy.UI — Clay-based immediate-mode UI layout + GPU rendering.
#
# Built on top of Sturdy.Renderer's existing GPU text stack (TextAtlas/TextPipeline) instead of a
# second one, and on Clay (Sturdy::clay) for layout. Deliberately does NOT depend on Sturdy.Ecs —
# usable identically from a bare game loop, an ECS system, or editor tooling. See
# plans/clay-ui-renderer.md.
sturdy_add_package(UI
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Core
        Sturdy::RHI
        Sturdy::GLM
        Sturdy::Text
        Sturdy::Renderer
        Sturdy::clay

    PRIVATE_DEPS
        # SVG parsing/rasterization (UI/src/UI/Svg/SvgIcon.hpp) — an implementation detail behind
        # the plain RasterizedSvg struct, so lunasvg's own types never leak into UI's public headers.
        Sturdy::LunaSvg

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(UIDockWorkspaceTest tests/DockWorkspaceTest.cpp)
    target_link_libraries(UIDockWorkspaceTest PRIVATE Sturdy::UI)
    sturdy_enable_warnings(UIDockWorkspaceTest)
    add_test(NAME UIDockWorkspaceTest COMMAND UIDockWorkspaceTest)
    set_tests_properties(UIDockWorkspaceTest PROPERTIES TIMEOUT 10)

    add_executable(UIWidgetTest tests/WidgetTest.cpp)
    target_link_libraries(UIWidgetTest PRIVATE Sturdy::UI)
    sturdy_enable_warnings(UIWidgetTest)
    add_test(NAME UIWidgetTest COMMAND UIWidgetTest)
    set_tests_properties(UIWidgetTest PROPERTIES TIMEOUT 10)

    add_executable(UIWidgetCompositionTest tests/WidgetCompositionTest.cpp)
    target_link_libraries(UIWidgetCompositionTest PRIVATE Sturdy::UI)
    sturdy_enable_warnings(UIWidgetCompositionTest)
    add_test(NAME UIWidgetCompositionTest COMMAND UIWidgetCompositionTest)
    set_tests_properties(UIWidgetCompositionTest PROPERTIES TIMEOUT 10)

    # Same RUNTIME_OUTPUT_DIRECTORY sturdy_add_package(EXECUTABLE ...) sets for real demo targets
    # (SturdyPackage.cmake) — lets .zed/tasks.json's Compile/Run pattern (STURDY_TARGET=<name>
    # -DSTURDY_RUN=ON, cmake/IDEBuild.cmake) address these test binaries by name identically to
    # KeyboardTester, instead of only being reachable through ctest.
    set_target_properties(UIDockWorkspaceTest UIWidgetTest UIWidgetCompositionTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
