sturdy_add_package(Ecs
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::Tracy

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(EcsQueryChunkTest tests/QueryChunkTest.cpp)
    target_link_libraries(EcsQueryChunkTest PRIVATE Sturdy::Ecs)
    sturdy_enable_warnings(EcsQueryChunkTest)
    add_test(NAME EcsQueryChunkTest COMMAND EcsQueryChunkTest)
    set_tests_properties(EcsQueryChunkTest PROPERTIES TIMEOUT 20)

    add_executable(EcsSynchronousExecutorTest tests/SynchronousExecutorTest.cpp)
    target_link_libraries(EcsSynchronousExecutorTest PRIVATE Sturdy::Ecs)
    sturdy_enable_warnings(EcsSynchronousExecutorTest)
    add_test(NAME EcsSynchronousExecutorTest COMMAND EcsSynchronousExecutorTest)
    set_tests_properties(EcsSynchronousExecutorTest PROPERTIES TIMEOUT 20)

    set(
        _sturdy_ecs_dependency_boundary_test
        "${CMAKE_SOURCE_DIR}/cmake/tests/AssertEcsDependencyBoundary.cmake"
    )
    add_test(
        NAME EcsDependencyBoundaryTest
        COMMAND "${CMAKE_COMMAND}"
            "-DSTURDY_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            -P "${_sturdy_ecs_dependency_boundary_test}"
    )
    set_tests_properties(
        EcsDependencyBoundaryTest
        PROPERTIES TIMEOUT 10
    )
endif()
