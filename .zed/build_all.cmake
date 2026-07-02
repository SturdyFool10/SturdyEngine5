cmake_minimum_required(VERSION 3.20)

# Configures once with Ninja Multi-Config, then builds every native (host-OS)
# profile: Debug, RelWithDebInfo, Release, Dist. This avoids four separate
# dependency/configure/generate passes while keeping all dependency targets enabled.

get_filename_component(_script_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_root "${_script_dir}/.." ABSOLUTE)

set(_configure_preset ninja-multi)
set(_build_presets
    runtime-multi-debug
    runtime-multi-relwithdebinfo
    runtime-multi-release
    runtime-multi-dist
)

message(STATUS "==> Configuring ${_configure_preset}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --preset "${_configure_preset}"
    WORKING_DIRECTORY "${_root}"
    RESULT_VARIABLE _configure_result
)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "CMake configure failed for preset ${_configure_preset} with exit code ${_configure_result}.")
endif()

foreach(_build_preset IN LISTS _build_presets)
    message(STATUS "==> Building ${_build_preset}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build --preset "${_build_preset}" --parallel
        WORKING_DIRECTORY "${_root}"
        RESULT_VARIABLE _build_result
    )
    if(NOT _build_result EQUAL 0)
        message(FATAL_ERROR "CMake build failed for preset ${_build_preset} with exit code ${_build_result}.")
    endif()
endforeach()

message(STATUS "==> Build All: all native profiles built successfully.")
