sturdy_add_package(Async
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation

    DEBUG_PUBLIC_DEPS
)

# Async deliberately depends on nothing but Foundation, so it gets its own copy of just the one
# platform bit it actually needs (STURDY_PLATFORM_WEB, to pick SynchronousRuntime as DefaultRuntime
# in :Runtime) rather than pulling in the whole Platform package for it — see Platform/CMakeLists.txt
# for the equivalent, fuller platform-definitions block.
if(STURDY_OS STREQUAL "Web")
    target_compile_definitions(Async PUBLIC STURDY_PLATFORM_WEB=1)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(AsyncSchedulerTest tests/SchedulerTest.cpp)
    target_link_libraries(AsyncSchedulerTest PRIVATE Sturdy::Async)
    sturdy_enable_warnings(AsyncSchedulerTest)
    add_test(NAME AsyncSchedulerTest COMMAND AsyncSchedulerTest)
    set_tests_properties(AsyncSchedulerTest PROPERTIES TIMEOUT 10)
endif()
