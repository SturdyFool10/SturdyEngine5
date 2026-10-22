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
set(_sturdy_ffi_static_only_tests AbiContractTest EcsFfiTest AnimationFfiTest AudioFfiTest)
# PluginFfiTest is registered separately below: it needs the path of the fixture plugin it loads.
set(_sturdy_ffi_excluded_tests PluginFfiTest)
if(STURDY_BUILD_SHARED_LIBS)
    list(APPEND _sturdy_ffi_excluded_tests ${_sturdy_ffi_static_only_tests})
endif()
sturdy_add_tests(FFI
    TIMEOUT 30
    EXCLUDE ${_sturdy_ffi_excluded_tests}
    NAMES
        AbiContractTest=FfiContractTest
        EcsFfiTest=FfiEcsTest
        AnimationFfiTest=FfiAnimationTest
        AudioFfiTest=FfiAudioTest
        ReflectionFfiTest=FfiReflectionTest
        TraceFfiTest=FfiTraceTest)

# Native plugins: a plugin is a shared library that exports `sturdy_plugin_entry` and knows the engine only through Sturdy.h, so the fixture
# is plain C built as a loadable module (like the hot-reload fixtures) and the test opens it with a real dlopen/LoadLibrary. Static-only for
# the same reason as the other tests that mint an engine handle.
if(STURDY_TESTS_ENABLED AND NOT STURDY_BUILD_SHARED_LIBS)
    add_library(FfiPluginFixture MODULE tests/fixtures/PluginFixture.c)
    set_target_properties(FfiPluginFixture PROPERTIES C_VISIBILITY_PRESET hidden C_STANDARD 99)
    target_include_directories(FfiPluginFixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    sturdy_add_test(FFI tests/PluginFfiTest.cpp NAME FfiPluginTest TIMEOUT 30
        DEPENDS FfiPluginFixture
        DEFINES "STURDY_PLUGIN_FIXTURE=\"$<TARGET_FILE:FfiPluginFixture>\"")

    # The same plugin written in Rust (rustc directly: no cargo, no engine crate), run through the same test. Skipped without rustc.
    find_program(STURDY_RUSTC rustc)
    if(STURDY_RUSTC)
        set(_rust_plugin "${CMAKE_CURRENT_BINARY_DIR}/rust_plugin/${CMAKE_SHARED_LIBRARY_PREFIX}plugin_fixture${CMAKE_SHARED_LIBRARY_SUFFIX}")
        add_custom_command(OUTPUT "${_rust_plugin}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rust_plugin"
            COMMAND ${STURDY_RUSTC} --edition 2021 --crate-type cdylib -C panic=abort -O
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/plugin_fixture.rs" -o "${_rust_plugin}"
            DEPENDS tests/fixtures/plugin_fixture.rs
            COMMENT "Building the Rust plugin fixture")
        add_custom_target(FfiPluginRustFixture DEPENDS "${_rust_plugin}")
        add_dependencies(FfiPluginTest FfiPluginRustFixture)
        add_test(NAME FfiPluginRustTest COMMAND FfiPluginTest "${_rust_plugin}" rust)
        set_tests_properties(FfiPluginRustTest PROPERTIES TIMEOUT 30)
    else()
        message(STATUS "FfiPluginRustTest skipped: rustc not found")
    endif()

    # And written with the `sturdy-plugin` crate (Rust/sturdy-plugin, over the generated FFI/bindings/rust declarations), which is what a real
    # Rust game module would use. Built with cargo into the build tree; skipped without cargo.
    find_program(STURDY_CARGO cargo)
    if(STURDY_CARGO)
        set(_rust_safe_target "${CMAKE_CURRENT_BINARY_DIR}/rust_target")
        set(_rust_safe_plugin "${_rust_safe_target}/release/${CMAKE_SHARED_LIBRARY_PREFIX}plugin_fixture${CMAKE_SHARED_LIBRARY_SUFFIX}")
        sturdy_glob(_rust_safe_sources RECURSE "${PROJECT_SOURCE_DIR}/Rust/sturdy-plugin/src/*.rs" "${PROJECT_SOURCE_DIR}/Rust/examples/plugin_fixture/src/*.rs")
        add_custom_command(OUTPUT "${_rust_safe_plugin}"
            COMMAND ${STURDY_CARGO} build --offline --release -p plugin_fixture --target-dir "${_rust_safe_target}"
                    --manifest-path "${PROJECT_SOURCE_DIR}/Rust/Cargo.toml"
            DEPENDS ${_rust_safe_sources} "${CMAKE_CURRENT_SOURCE_DIR}/bindings/rust/sturdy_ffi.rs"
            COMMENT "Building the sturdy-plugin fixture")
        add_custom_target(FfiPluginSafeRustFixture DEPENDS "${_rust_safe_plugin}")
        add_dependencies(FfiPluginTest FfiPluginSafeRustFixture)
        add_test(NAME FfiPluginSafeRustTest COMMAND FfiPluginTest "${_rust_safe_plugin}" rust all-callbacks)
        set_tests_properties(FfiPluginSafeRustTest PROPERTIES TIMEOUT 120)
    else()
        message(STATUS "FfiPluginSafeRustTest skipped: cargo not found")
    endif()
endif()

# Language bindings for the ABI (FFI/bindings/: Python, Java, C#) are generated from Sturdy.h by FFI/tools/ (describe_abi.py reads the
# header with clang, gen_bindings.py writes each language). This test regenerates them and fails when a committed file differs, so a
# header change that skips regeneration cannot land, then checks the Python and Java bindings against the real header's struct
# layouts and by calling every function through a stub library. It needs only Python and clang (Java 22+ is used when present).
if(STURDY_TESTS_ENABLED)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    find_program(STURDY_BINDINGS_CLANG NAMES clang clang-20 clang-19 clang-18)
    if(Python3_Interpreter_FOUND AND STURDY_BINDINGS_CLANG)
        add_test(NAME FfiBindingsTest
            COMMAND ${CMAKE_COMMAND} -E env CLANG=${STURDY_BINDINGS_CLANG}
                    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/bindings/check_bindings.py --repo ${PROJECT_SOURCE_DIR})
        set_tests_properties(FfiBindingsTest PROPERTIES TIMEOUT 300)
    else()
        message(STATUS "FfiBindingsTest skipped: needs Python 3 and clang")
    endif()
endif()
