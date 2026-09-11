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
# depends on nothing (not even WindowManager) — same rationale as Async's own STURDY_PLATFORM_WEB
# definition below Async's sturdy_add_package() call: re-derive just the one platform bit actually
# needed here rather than pulling in the whole WindowManager package for it.
if(STURDY_OS STREQUAL "Windows")
    target_compile_definitions(Foundation PUBLIC STURDY_PLATFORM_WINDOWS=1)
endif()

# DynamicLibrary.cpp's dlopen/dlsym/dlclose need libdl on platforms where those symbols aren't
# already folded into libc (older glibc, most non-Linux Unix-likes); CMAKE_DL_LIBS resolves to the
# right thing per-platform (including empty on Windows/newer glibc, where it's a harmless no-op).
target_link_libraries(Foundation PRIVATE ${CMAKE_DL_LIBS})

# Foundation is a static library by default (STURDY_BUILD_SHARED_LIBS is OFF), but a
# hot-reloadable game-logic module (Engine::HotReloadableModule) is always a real shared library
# regardless of that toggle, and links Foundation in. Non-PIC static-library objects cannot be
# linked into a shared object at all (relocation errors at link time) — PIC objects link into
# ordinary non-PIC executables just fine, so enabling this here is safe for every other consumer,
# at the small, usually-negligible cost of one extra GOT/PLT indirection for global data access.
set_target_properties(Foundation PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(FoundationConsoleDiagnosticTest tests/ConsoleDiagnosticTest.cpp)
    target_link_libraries(FoundationConsoleDiagnosticTest PRIVATE Sturdy::Foundation)
    sturdy_enable_warnings(FoundationConsoleDiagnosticTest)
    add_test(NAME FoundationConsoleDiagnosticTest COMMAND FoundationConsoleDiagnosticTest)
    set_tests_properties(FoundationConsoleDiagnosticTest PROPERTIES TIMEOUT 10)

    add_executable(FoundationCpuTest tests/CpuTest.cpp)
    target_link_libraries(FoundationCpuTest PRIVATE Sturdy::Foundation)
    sturdy_enable_warnings(FoundationCpuTest)
    add_test(NAME FoundationCpuTest COMMAND FoundationCpuTest)
    set_tests_properties(FoundationCpuTest PROPERTIES TIMEOUT 10)

    add_executable(FoundationCoreMapTest tests/CoreMapTest.cpp)
    target_link_libraries(FoundationCoreMapTest PRIVATE Sturdy::Foundation)
    sturdy_enable_warnings(FoundationCoreMapTest)
    add_test(NAME FoundationCoreMapTest COMMAND FoundationCoreMapTest)
    set_tests_properties(FoundationCoreMapTest PROPERTIES TIMEOUT 10)
endif()
