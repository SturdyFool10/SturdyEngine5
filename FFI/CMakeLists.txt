# Sturdy.FFI — the stable C ABI every non-C++ consumer (Rust, C#, Java, ...) binds against.
#
# Deliberately a leaf: nothing in the engine may depend on this package. It is a translation layer
# in the same spirit as an RHI backend — the features live in the C++ stack below, and this only
# restates them in a form a foreign language can call. Widening it means adding more translation
# here, never moving logic into it.
sturdy_add_package(FFI
    PUBLIC_DEPS
        Sturdy::Runtime

    PRIVATE_DEFINES
        # Selects the exporting half of STURDY_ABI in Sturdy.h. Consumers of this header (including
        # the tests below, which link the target and therefore see only the PUBLIC define) get the
        # importing half instead.
        STURDY_FFI_BUILD
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

# Compiled as C, not C++. This is the only thing that actually proves the header is valid C and
# free of C++-only constructs — compiling it as C++ (which every other test does implicitly)
# would happily accept declarations no C consumer could use.
sturdy_add_test(FFI tests/AbiHeaderC.c NAME FfiHeaderCTest TIMEOUT 30 C_STANDARD 99)

# Brings up a real window and graphics device, so it needs a GPU and a display that headless CI
# does not have — always built (it also doubles as the worked C example for the native-access
# surface), but only registered with add_test() below when STURDY_ENABLE_GPU_TESTS is ON. Run it
# by hand ("FfiNativeHandleProbe vulkan" / "... d3d12") to see what the RHI and native-handle
# queries actually return on a given machine.
sturdy_add_test(FFI tests/NativeHandleProbe.c NAME FfiNativeHandleProbe C_STANDARD 99 NO_TEST)

if(STURDY_TESTS_ENABLED AND STURDY_ENABLE_GPU_TESTS)
    # Exercises real shader compilation, bind-group-layout/pipeline creation and a few rendered
    # frames end to end on each backend — the one thing the rest of the FFI test suite can't
    # touch without a GPU. Exits nonzero (see `g_scene_ok` in NativeHandleProbe.c) if the scene
    # build fails on either backend, so a regression here fails the build instead of only
    # showing up as a log line nobody reads.
    add_test(NAME FfiNativeHandleProbeVulkan COMMAND FfiNativeHandleProbe vulkan)
    set_tests_properties(FfiNativeHandleProbeVulkan PROPERTIES TIMEOUT 120)
    if(STURDY_OS STREQUAL "Windows")
        add_test(NAME FfiNativeHandleProbeD3D12 COMMAND FfiNativeHandleProbe d3d12)
        set_tests_properties(FfiNativeHandleProbeD3D12 PROPERTIES TIMEOUT 120)
    endif()
    # Web itself has no ctest runner (no browser in CI); this only covers WebGPU-via-Dawn on a
    # native desktop, same restriction STURDY_ENABLE_WEBGPU already carries everywhere else.
    if(STURDY_ENABLE_WEBGPU)
        add_test(NAME FfiNativeHandleProbeWebGpu COMMAND FfiNativeHandleProbe webgpu)
        set_tests_properties(FfiNativeHandleProbeWebGpu PROPERTIES TIMEOUT 120)
    endif()
endif()

# AbiContractTest reaches the internal handle table to construct an expired handle, which the public
# header deliberately makes impossible, and EcsFfiTest mints its engine handle the same way. Those
# symbols carry no STURDY_ABI marking and so are hidden in a shared build — correctly, since they
# are not part of the ABI — leaving both buildable only for static configurations. (The ECS is pure
# CPU state, so EcsFfiTest exercises real behavior, not just argument validation, with no GPU or
# window.) ReflectionFfiTest needs neither: TypeRegistry is a process-wide singleton rather than
# something owned by a SturdyEngine session, so it builds in both configurations.
set(_sturdy_ffi_static_only_tests AbiContractTest EcsFfiTest)
set(_sturdy_ffi_excluded_tests)
if(STURDY_BUILD_SHARED_LIBS)
    set(_sturdy_ffi_excluded_tests ${_sturdy_ffi_static_only_tests})
endif()
sturdy_add_tests(FFI
    TIMEOUT 30
    EXCLUDE ${_sturdy_ffi_excluded_tests}
    NAMES
        AbiContractTest=FfiContractTest
        EcsFfiTest=FfiEcsTest
        ReflectionFfiTest=FfiReflectionTest)
