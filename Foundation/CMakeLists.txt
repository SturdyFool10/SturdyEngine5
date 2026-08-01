sturdy_add_package(Foundation
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::fmt
        Sturdy::spdlog
    PRIVATE_DEPS
        Sturdy::mimalloc
)

# Cli.hpp's Windows command-line tokenizer needs STURDY_PLATFORM_WINDOWS, but Foundation deliberately
# depends on nothing (not even Platform) — same rationale as Async's own STURDY_PLATFORM_WEB
# definition below Async's sturdy_add_package() call: re-derive just the one platform bit actually
# needed here rather than pulling in the whole Platform package for it.
if(STURDY_OS STREQUAL "Windows")
    target_compile_definitions(Foundation PUBLIC STURDY_PLATFORM_WINDOWS=1)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(FoundationConsoleDiagnosticTest tests/ConsoleDiagnosticTest.cpp)
    target_link_libraries(FoundationConsoleDiagnosticTest PRIVATE Sturdy::Foundation)
    sturdy_enable_warnings(FoundationConsoleDiagnosticTest)
    add_test(NAME FoundationConsoleDiagnosticTest COMMAND FoundationConsoleDiagnosticTest)
    set_tests_properties(FoundationConsoleDiagnosticTest PROPERTIES TIMEOUT 10)
endif()
