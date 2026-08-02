# Interactive UI showcase kept as reusable game logic so Runtime and future editor hosts can
# launch the same workbench without owning its widget or docking state.
sturdy_add_package(UiWorkbenchGameLogic
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Engine
        Sturdy::Text
        Sturdy::UI

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(UiWorkbenchGameLogicFactoryTest tests/FactoryTest.cpp)
    target_link_libraries(UiWorkbenchGameLogicFactoryTest PRIVATE Sturdy::UiWorkbenchGameLogic)
    sturdy_enable_warnings(UiWorkbenchGameLogicFactoryTest)
    add_test(NAME UiWorkbenchGameLogicFactoryTest COMMAND UiWorkbenchGameLogicFactoryTest)
    set_tests_properties(UiWorkbenchGameLogicFactoryTest PROPERTIES TIMEOUT 10)
endif()
