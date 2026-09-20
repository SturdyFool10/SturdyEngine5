# Interactive UI showcase kept as reusable game logic so Runtime and future editor hosts can
# launch the same workbench without owning its widget or docking state.
set(_sturdy_ui_workbench_optional_deps)
set(_sturdy_ui_workbench_defines)
if(STURDY_BUILD_GLFW_WINDOW_PROVIDER)
    list(APPEND _sturdy_ui_workbench_optional_deps Sturdy::WindowManagerGLFW)
    list(APPEND _sturdy_ui_workbench_defines STURDY_UI_WORKBENCH_HAS_GLFW=1)
endif()

sturdy_add_package(UiWorkbenchGameLogic
    PUBLIC_DEPS
        Sturdy::Engine
        Sturdy::Renderer
        ${_sturdy_ui_workbench_optional_deps}

    PRIVATE_DEFINES
        ${_sturdy_ui_workbench_defines}
)

sturdy_add_tests(UiWorkbenchGameLogic)
