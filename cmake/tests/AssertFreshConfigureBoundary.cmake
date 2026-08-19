cmake_minimum_required(VERSION 3.28)

# Generic pay-for-use build-graph probe: configure a fresh, isolated binary tree with one option
# forced off and assert the resulting build.ninja contains no trace of the dependency that option
# is supposed to remove. Source-text scans (see AssertBaseDependencyBoundary.cmake) only work for
# dependencies that must never appear anywhere, like Box3D; GLFW/Runtime-demo-style options are
# legitimately mentioned in CMake source under their own `if()` guard, so the only real proof of
# "this option actually removes the dependency" is inspecting what a real configure produced.
foreach(_required_var
    STURDY_SOURCE_DIR
    STURDY_PROBE_LABEL
    STURDY_PROBE_OPTION
    STURDY_PROBE_FORBIDDEN_STRING
    STURDY_PROBE_ARCH
    STURDY_PROBE_OS
    STURDY_PROBE_COMPILER
    STURDY_PROBE_C_COMPILER
    STURDY_PROBE_CXX_COMPILER
)
    if(NOT DEFINED ${_required_var})
        message(FATAL_ERROR "${_required_var} must be set to run this boundary probe.")
    endif()
endforeach()

set(_probe_binary_dir "${STURDY_SOURCE_DIR}/build/_boundary_probe/${STURDY_PROBE_LABEL}")
file(REMOVE_RECURSE "${_probe_binary_dir}")
file(MAKE_DIRECTORY "${_probe_binary_dir}")

# Dependency sources land in the shared STURDY_DEPS_CACHE_DIR (repo-root .cache/deps) regardless of
# binary dir, so this fresh configure reuses whatever the primary build tree already fetched instead
# of re-downloading — it only re-runs CMake's own configure/generate step.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${STURDY_SOURCE_DIR}"
        -B "${_probe_binary_dir}"
        -G Ninja
        "-DCMAKE_C_COMPILER=${STURDY_PROBE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER=${STURDY_PROBE_CXX_COMPILER}"
        "-DSTURDY_ARCH=${STURDY_PROBE_ARCH}"
        "-DSTURDY_OS=${STURDY_PROBE_OS}"
        "-DSTURDY_COMPILER=${STURDY_PROBE_COMPILER}"
        "-D${STURDY_PROBE_OPTION}"
        -DBUILD_TESTING=OFF
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error
)

if(NOT _configure_result EQUAL 0)
    file(REMOVE_RECURSE "${_probe_binary_dir}")
    message(FATAL_ERROR
        "Fresh configure for boundary probe '${STURDY_PROBE_LABEL}' (-D${STURDY_PROBE_OPTION}) failed:\n"
        "${_configure_output}\n${_configure_error}"
    )
endif()

set(_ninja_file "${_probe_binary_dir}/build.ninja")
if(NOT EXISTS "${_ninja_file}")
    file(REMOVE_RECURSE "${_probe_binary_dir}")
    message(FATAL_ERROR "Boundary probe '${STURDY_PROBE_LABEL}' produced no build.ninja to inspect.")
endif()

file(READ "${_ninja_file}" _ninja_contents)

# build.ninja is full of absolute paths rooted at _probe_binary_dir. If STURDY_PROBE_LABEL (or
# anything else in that path) happens to contain the forbidden string itself, every single build
# statement would false-positive on the probe's own directory name rather than a real dependency
# reference — strip it before searching so only genuine content (e.g. a real
# .cache/deps/<dep>-src path, which lives outside this probe's own binary dir) can match.
string(REPLACE "${_probe_binary_dir}" "" _ninja_contents_stripped "${_ninja_contents}")

string(TOLOWER "${_ninja_contents_stripped}" _ninja_contents_lower)
string(TOLOWER "${STURDY_PROBE_FORBIDDEN_STRING}" _forbidden_lower)
string(FIND "${_ninja_contents_lower}" "${_forbidden_lower}" _forbidden_index)

file(REMOVE_RECURSE "${_probe_binary_dir}")

if(NOT _forbidden_index EQUAL -1)
    message(FATAL_ERROR
        "Boundary probe '${STURDY_PROBE_LABEL}' found forbidden string '${STURDY_PROBE_FORBIDDEN_STRING}' "
        "in the generated build graph after configuring with -D${STURDY_PROBE_OPTION} — the disabled "
        "option did not actually remove the dependency from the build."
    )
endif()

message(STATUS
    "Boundary probe '${STURDY_PROBE_LABEL}' confirmed the build graph has no '${STURDY_PROBE_FORBIDDEN_STRING}' "
    "reference with -D${STURDY_PROBE_OPTION}."
)
