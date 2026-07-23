sturdy_add_package(Ecs
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(EcsQueryChunkTest tests/QueryChunkTest.cpp)
    target_link_libraries(EcsQueryChunkTest PRIVATE Sturdy::Ecs)
    sturdy_enable_warnings(EcsQueryChunkTest)
    add_test(NAME EcsQueryChunkTest COMMAND EcsQueryChunkTest)
    set_tests_properties(EcsQueryChunkTest PROPERTIES TIMEOUT 20)
endif()
