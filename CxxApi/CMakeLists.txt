# Sturdy.CxxApi — the engine's C++ API restated in the subset of C++ that the Rust `cxx` crate (and
# bindgen for the plain-data types) can bind directly. See plans/cxx-api.md for the rules every
# declaration follows.
#
# A leaf like FFI: nothing in the engine depends on it. It only translates; features live below.
sturdy_add_package(CxxApi
    PUBLIC_DEPS
        Sturdy::Runtime
)

sturdy_add_tests(CxxApi TIMEOUT 60)

# The Rust repo authors and generates its own `cxx` bridge. This test only checks CxxApi stays bindable: it fails when a declared function
# has no definition, then runs a throwaway bridge built from the headers through the real cxx toolchain (bindgen for
# the plain-data types, `cargo check`, cxx-gen, and a compile of the generated C++ against the engine's headers) when cargo and bindgen
# are installed. It reads the headers with clang's AST, so it needs a clang build and the compile_commands.json the presets export.
if(STURDY_TESTS_ENABLED AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_EXPORT_COMPILE_COMMANDS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME CxxApiBridgeTest
            COMMAND ${CMAKE_COMMAND} -E env CLANGXX=${CMAKE_CXX_COMPILER}
                    ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_bridge.py
                    --repo ${PROJECT_SOURCE_DIR}
                    --compile-commands ${CMAKE_BINARY_DIR}/compile_commands.json
                    --work-dir ${CMAKE_BINARY_DIR}/cxxapi-bridge-cargo)
        set_tests_properties(CxxApiBridgeTest PROPERTIES TIMEOUT 600)
    endif()
endif()
