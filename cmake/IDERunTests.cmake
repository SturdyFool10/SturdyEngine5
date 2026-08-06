# Generic, editor-agnostic "build everything, run the full test suite" bridge — the ctest
# counterpart to IDEBuild.cmake (see that file's header comment for the overall rationale). Kept as
# its own script rather than a mode of IDEBuild.cmake since it needs a full build (every test
# binary, not one STURDY_TARGET) and ctest instead of launching a single executable.
#
#   cmake [-DSTURDY_ARCH=<arch>] [-DSTURDY_OS=<os>] [-DSTURDY_COMPILER=<compiler>] \
#         [-DSTURDY_PROFILE=<profile>] -P cmake/IDERunTests.cmake
#
# STURDY_ARCH / STURDY_OS default to the host; STURDY_COMPILER defaults to Clang; STURDY_PROFILE
# defaults to Debug — same defaulting as IDEBuild.cmake. ctest runs serially (no -j): the two
# `*OffDependencyBoundaryTest` cases race each other's own isolated CMake sub-builds when run in
# parallel and report spurious failures — serial execution (ctest's own default) is what actually
# avoids that, not a hardcoded skip list.
cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/SturdyMatrix.cmake")
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_LIST_DIR}/GeneratePresets.cmake"
    WORKING_DIRECTORY "${_root}"
    RESULT_VARIABLE _presets_result
)
if(NOT _presets_result EQUAL 0)
    message(FATAL_ERROR "Failed to generate CMakePresets.json (exit ${_presets_result}).")
endif()

sturdy_detect_host_arch(_host_arch)
sturdy_detect_host_os(_host_os)

if(NOT DEFINED STURDY_ARCH OR STURDY_ARCH STREQUAL "")
    set(STURDY_ARCH "${_host_arch}")
endif()
if(NOT DEFINED STURDY_OS OR STURDY_OS STREQUAL "")
    set(STURDY_OS "${_host_os}")
endif()
if(NOT DEFINED STURDY_COMPILER OR STURDY_COMPILER STREQUAL "")
    set(STURDY_COMPILER "Clang")
endif()
if(NOT DEFINED STURDY_PROFILE OR STURDY_PROFILE STREQUAL "")
    set(STURDY_PROFILE "Debug")
endif()

if(NOT STURDY_ARCH IN_LIST STURDY_ARCH_LIST)
    message(FATAL_ERROR "Unsupported STURDY_ARCH='${STURDY_ARCH}'. Expected one of: ${STURDY_ARCH_LIST}")
endif()
if(NOT STURDY_OS IN_LIST STURDY_OS_LIST)
    message(FATAL_ERROR "Unsupported STURDY_OS='${STURDY_OS}'. Expected one of: ${STURDY_OS_LIST}")
endif()
if(NOT STURDY_COMPILER IN_LIST STURDY_COMPILER_LIST)
    message(FATAL_ERROR "Unsupported STURDY_COMPILER='${STURDY_COMPILER}'. Expected one of: ${STURDY_COMPILER_LIST}")
endif()
if(NOT STURDY_PROFILE IN_LIST STURDY_PROFILE_LIST)
    message(FATAL_ERROR "Unsupported STURDY_PROFILE='${STURDY_PROFILE}'. Expected one of: ${STURDY_PROFILE_LIST}")
endif()

sturdy_preset_name("${STURDY_ARCH}" "${STURDY_OS}" "${STURDY_PROFILE}" "${STURDY_COMPILER}" _preset)
sturdy_binary_dir("${STURDY_ARCH}" "${STURDY_OS}" "${STURDY_PROFILE}" "${STURDY_COMPILER}" _binary_dir)

message(STATUS "==> Configuring ${_preset}")
execute_process(
    # STURDY_FETCH_SAMPLE_ASSETS=ON for the same reason IDEBuild.cmake always sets it: one shared
    # configure step backs every editor task, so it shouldn't silently diverge from what Compile/Run
    # tasks already configured for this preset.
    COMMAND "${CMAKE_COMMAND}" --preset "${_preset}" -DSTURDY_FETCH_SAMPLE_ASSETS=ON
    WORKING_DIRECTORY "${_root}"
    RESULT_VARIABLE _configure_result
)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "CMake configure failed for preset '${_preset}' (exit ${_configure_result}).")
endif()

message(STATUS "==> Building all targets (${_preset})")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_root}/${_binary_dir}" --parallel
    WORKING_DIRECTORY "${_root}"
    RESULT_VARIABLE _build_result
)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "CMake build failed for preset '${_preset}' (exit ${_build_result}).")
endif()

message(STATUS "==> Running ctest (${_preset})")
execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
    WORKING_DIRECTORY "${_root}/${_binary_dir}"
    RESULT_VARIABLE _ctest_result
)
if(NOT _ctest_result EQUAL 0)
    message(FATAL_ERROR "ctest reported failures (exit ${_ctest_result}).")
endif()
