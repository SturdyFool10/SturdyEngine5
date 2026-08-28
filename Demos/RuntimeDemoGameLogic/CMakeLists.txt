# Sample game/application logic kept in its own archive so Runtime and a future Editor can both
# reference the same explicit factory without linking either product host into the other.
sturdy_add_package(RuntimeDemoGameLogic
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Engine

    DEBUG_PUBLIC_DEPS
)

# Only the demo logic consumes the optional Khronos sample asset checkout. The reusable host and
# unrelated products never inherit this source-tree definition.
if(STURDY_GLTF_SAMPLE_ASSETS_DIR)
    target_compile_definitions(RuntimeDemoGameLogic PRIVATE
        "STURDY_GLTF_SAMPLE_ASSETS_DIR=\"${STURDY_GLTF_SAMPLE_ASSETS_DIR}\"")
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(RuntimeDemoGameLogicFactoryTest tests/FactoryTest.cpp)
    target_link_libraries(RuntimeDemoGameLogicFactoryTest PRIVATE Sturdy::RuntimeDemoGameLogic)
    sturdy_enable_warnings(RuntimeDemoGameLogicFactoryTest)
    add_test(NAME RuntimeDemoGameLogicFactoryTest COMMAND RuntimeDemoGameLogicFactoryTest)
    set_tests_properties(RuntimeDemoGameLogicFactoryTest PROPERTIES TIMEOUT 10)

    if(NOT STURDY_BUILD_SHARED_LIBS)
        add_executable(
            RuntimeDemoGameLogicUnusedProbe
            "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp"
        )
        target_link_libraries(RuntimeDemoGameLogicUnusedProbe PRIVATE Sturdy::RuntimeDemoGameLogic)
        sturdy_enable_warnings(RuntimeDemoGameLogicUnusedProbe)
        add_test(
            NAME RuntimeDemoGameLogicUnusedTest
            COMMAND "${CMAKE_COMMAND}"
                "-DSTURDY_STATIC_PROBE=$<TARGET_FILE:RuntimeDemoGameLogicUnusedProbe>"
                -P "${CMAKE_SOURCE_DIR}/cmake/tests/AssertStaticAggregateUnused.cmake"
        )
        set_tests_properties(RuntimeDemoGameLogicUnusedTest PROPERTIES TIMEOUT 60)
    endif()
endif()
