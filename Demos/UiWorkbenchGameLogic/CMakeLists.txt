# Interactive UI showcase kept as reusable game logic so Runtime and future editor hosts can
# launch the same workbench without owning its widget or docking state.
set(_sturdy_ui_workbench_optional_deps)
set(_sturdy_ui_workbench_defines)
if(STURDY_BUILD_GLFW_WINDOW_PROVIDER)
    list(APPEND _sturdy_ui_workbench_optional_deps Sturdy::WindowManagerGLFW)
    list(APPEND _sturdy_ui_workbench_defines STURDY_UI_WORKBENCH_HAS_GLFW=1)
endif()

sturdy_add_package(UiWorkbenchGameLogic
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Engine
        Sturdy::Renderer
        ${_sturdy_ui_workbench_optional_deps}

    PRIVATE_DEFINES
        ${_sturdy_ui_workbench_defines}

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(UiWorkbenchGameLogicFactoryTest tests/FactoryTest.cpp)
    target_link_libraries(UiWorkbenchGameLogicFactoryTest PRIVATE Sturdy::UiWorkbenchGameLogic)
    sturdy_enable_warnings(UiWorkbenchGameLogicFactoryTest)
    add_test(NAME UiWorkbenchGameLogicFactoryTest COMMAND UiWorkbenchGameLogicFactoryTest)
    set_tests_properties(UiWorkbenchGameLogicFactoryTest PROPERTIES TIMEOUT 10)
endif()
