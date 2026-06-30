cmake_minimum_required(VERSION 3.20)

# Configures and builds every native (host-OS) profile in sequence: Debug,
# RelWithDebInfo, Dist. Uses the ninja-* presets (not the cross-platform
# "view-as" ones), so this always targets whatever toolchain is installed
# on the machine running it — no OS-specific branching needed.

get_filename_component(_script_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_root "${_script_dir}/.." ABSOLUTE)

set(_configure_presets ninja-debug          ninja-relwithdebinfo          ninja-dist)
set(_build_presets     runtime-debug        runtime-relwithdebinfo        runtime-dist)

list(LENGTH _configure_presets _count)
math(EXPR _last_index "${_count} - 1")

foreach(_i RANGE 0 ${_last_index})
    list(GET _configure_presets ${_i} _configure_preset)
    list(GET _build_presets ${_i} _build_preset)

    message(STATUS "==> Configuring ${_configure_preset}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --preset "${_configure_preset}"
        WORKING_DIRECTORY "${_root}"
        RESULT_VARIABLE _configure_result
    )
    if(NOT _configure_result EQUAL 0)
        message(FATAL_ERROR "CMake configure failed for preset ${_configure_preset} with exit code ${_configure_result}.")
    endif()

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
