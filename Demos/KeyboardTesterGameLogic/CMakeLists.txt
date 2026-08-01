# Sample game/application logic kept in its own archive so Runtime and a future Editor can both
# reference the same explicit factory without linking either product host into the other — same
# reasoning as Demos/RuntimeDemoGameLogic's own CMakeLists.txt.
sturdy_add_package(KeyboardTesterGameLogic
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Engine

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(KeyboardTesterGameLogicFactoryTest tests/FactoryTest.cpp)
    target_link_libraries(KeyboardTesterGameLogicFactoryTest PRIVATE Sturdy::KeyboardTesterGameLogic)
    sturdy_enable_warnings(KeyboardTesterGameLogicFactoryTest)
    add_test(NAME KeyboardTesterGameLogicFactoryTest COMMAND KeyboardTesterGameLogicFactoryTest)
    set_tests_properties(KeyboardTesterGameLogicFactoryTest PROPERTIES TIMEOUT 10)

    if(NOT STURDY_BUILD_SHARED_LIBS)
        add_executable(
            KeyboardTesterGameLogicUnusedProbe
            "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp"
        )
        target_link_libraries(KeyboardTesterGameLogicUnusedProbe PRIVATE Sturdy::KeyboardTesterGameLogic)
        sturdy_enable_warnings(KeyboardTesterGameLogicUnusedProbe)
        add_test(
            NAME KeyboardTesterGameLogicUnusedTest
            COMMAND "${CMAKE_COMMAND}"
                "-DSTURDY_STATIC_PROBE=$<TARGET_FILE:KeyboardTesterGameLogicUnusedProbe>"
                -P "${CMAKE_SOURCE_DIR}/cmake/tests/AssertStaticAggregateUnused.cmake"
        )
        set_tests_properties(KeyboardTesterGameLogicUnusedTest PROPERTIES TIMEOUT 10)
    endif()
endif()
