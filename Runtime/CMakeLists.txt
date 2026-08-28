# Reusable standalone-application host. Runtime owns no demo content and selects no GameLogic
# implementation; products pass an explicit factory to Runtime::run().
sturdy_add_package(Runtime
    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Engine
        Sturdy::ApplicationHost

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web" AND NOT STURDY_BUILD_SHARED_LIBS)
    add_executable(
        RuntimeStaticUnusedProbe
        "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp"
    )
    target_link_libraries(RuntimeStaticUnusedProbe PRIVATE Sturdy::Runtime)
    sturdy_enable_warnings(RuntimeStaticUnusedProbe)
    add_test(
        NAME RuntimeStaticUnusedTest
        COMMAND "${CMAKE_COMMAND}"
            "-DSTURDY_STATIC_PROBE=$<TARGET_FILE:RuntimeStaticUnusedProbe>"
            -P "${CMAKE_SOURCE_DIR}/cmake/tests/AssertStaticAggregateUnused.cmake"
    )
    set_tests_properties(RuntimeStaticUnusedTest PROPERTIES TIMEOUT 60)
endif()
