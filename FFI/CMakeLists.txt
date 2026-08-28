# Sturdy.FFI — the stable C ABI every non-C++ consumer (Rust, C#, Java, ...) binds against.
#
# Deliberately a leaf: nothing in the engine may depend on this package. It is a translation layer
# in the same spirit as an RHI backend — the features live in the C++ stack below, and this only
# restates them in a form a foreign language can call. Widening it means adding more translation
# here, never moving logic into it.
sturdy_add_package(FFI
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Runtime

    PRIVATE_DEFINES
        # Selects the exporting half of STURDY_ABI in Sturdy.h. Consumers of this header (including
        # the tests below, which link the target and therefore see only the PUBLIC define) get the
        # importing half instead.
        STURDY_FFI_BUILD

    DEBUG_PUBLIC_DEPS
)

if(STURDY_BUILD_SHARED_LIBS)
    # Only meaningful for a real shared build: with a static library every symbol is resolved at
    # link time and dllimport/dllexport would be wrong on both sides.
    target_compile_definitions(FFI PUBLIC STURDY_FFI_SHARED)

    # Everything not explicitly marked STURDY_ABI stays internal to the library on ELF/Mach-O
    # targets, so the ABI surface is exactly the header and nothing leaks from the C++ stack below.
    # Windows already behaves this way: nothing is exported from a DLL without __declspec.
    set_target_properties(FFI PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    # Compiled as C, not C++. This is the only thing that actually proves the header is valid C and
    # free of C++-only constructs — compiling it as C++ (which every other test does implicitly)
    # would happily accept declarations no C consumer could use.
    add_executable(FfiHeaderCTest tests/AbiHeaderC.c)
    target_link_libraries(FfiHeaderCTest PRIVATE Sturdy::FFI)
    set_target_properties(FfiHeaderCTest PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    sturdy_enable_warnings(FfiHeaderCTest)
    add_test(NAME FfiHeaderCTest COMMAND FfiHeaderCTest)
    set_tests_properties(FfiHeaderCTest PROPERTIES TIMEOUT 30)

    # Manual verification tool, deliberately not registered with add_test(): it creates a real
    # window and graphics device, so it needs a GPU and a display that CI does not have. Run it by
    # hand ("FfiNativeHandleProbe vulkan" / "... d3d12") to see what the RHI and native-handle
    # queries actually return on a given machine. Also doubles as the worked C example for the
    # native-access surface.
    add_executable(FfiNativeHandleProbe tests/NativeHandleProbe.c)
    target_link_libraries(FfiNativeHandleProbe PRIVATE Sturdy::FFI)
    set_target_properties(FfiNativeHandleProbe PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    sturdy_enable_warnings(FfiNativeHandleProbe)

    # Reaches the internal handle table to construct an expired handle, which the public header
    # deliberately makes impossible. Those symbols carry no STURDY_ABI marking and so are hidden in
    # a shared build — correctly, since they are not part of the ABI — leaving this test buildable
    # only for static configurations.
    if(NOT STURDY_BUILD_SHARED_LIBS)
        # The ECS is pure CPU state, so unlike the rest of the FFI its real behavior - not just its
        # argument validation - can be exercised without a GPU or a window. Mints its engine handle
        # through the internal handle table, so it shares the static-only restriction below.
        add_executable(FfiEcsTest tests/EcsFfiTest.cpp)
        target_link_libraries(FfiEcsTest PRIVATE Sturdy::FFI)
        sturdy_enable_warnings(FfiEcsTest)
        add_test(NAME FfiEcsTest COMMAND FfiEcsTest)
        # Timeout kept short so a regression that hangs is caught too; the observed failure mode
        # for a missing reentrancy guard is actually a fatal engine contract check.
        set_tests_properties(FfiEcsTest PROPERTIES TIMEOUT 30)

        add_executable(FfiContractTest tests/AbiContractTest.cpp)
        target_link_libraries(FfiContractTest PRIVATE Sturdy::FFI)
        sturdy_enable_warnings(FfiContractTest)
        add_test(NAME FfiContractTest COMMAND FfiContractTest)
        set_tests_properties(FfiContractTest PROPERTIES TIMEOUT 30)
    endif()
endif()
