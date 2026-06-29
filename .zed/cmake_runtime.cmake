cmake_minimum_required(VERSION 3.20)

get_filename_component(_script_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_root "${_script_dir}/.." ABSOLUTE)

if(NOT DEFINED STURDY_RUNTIME_PROFILE)
    message(FATAL_ERROR "STURDY_RUNTIME_PROFILE is required.")
endif()

if(STURDY_RUNTIME_PROFILE STREQUAL "debug")
    set(_configure_preset "ninja-debug")
    set(_build_preset "runtime-debug")
    set(_runtime_path "build/ninja/debug/bin/Runtime")
    set(_windows_runtime_path "build/ninja/debug/bin/Runtime.exe")
elseif(STURDY_RUNTIME_PROFILE STREQUAL "relwithdebinfo")
    set(_configure_preset "ninja-relwithdebinfo")
    set(_build_preset "runtime-relwithdebinfo")
    set(_runtime_path "build/ninja/relwithdebinfo/bin/Runtime")
    set(_windows_runtime_path "build/ninja/relwithdebinfo/bin/Runtime.exe")
elseif(STURDY_RUNTIME_PROFILE STREQUAL "dist")
    set(_configure_preset "ninja-dist")
    set(_build_preset "runtime-dist")
    set(_runtime_path "build/ninja/dist/bin/Runtime")
    set(_windows_runtime_path "build/ninja/dist/bin/Runtime.exe")
else()
    message(FATAL_ERROR "Unsupported STURDY_RUNTIME_PROFILE: ${STURDY_RUNTIME_PROFILE}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --preset "${_configure_preset}"
    WORKING_DIRECTORY "${_root}"
    RESULT_VARIABLE _configure_result
)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "CMake configure failed with exit code ${_configure_result}.")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build --preset "${_build_preset}" --parallel
    WORKING_DIRECTORY "${_root}"
    RESULT_VARIABLE _build_result
)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "CMake build failed with exit code ${_build_result}.")
endif()

if(STURDY_RUNTIME_RUN)
    if(STURDY_RUNTIME_WINDOWS_EXE)
        set(_selected_runtime "${_windows_runtime_path}")
    else()
        set(_selected_runtime "${_runtime_path}")
    endif()

    set(_selected_runtime "${_root}/${_selected_runtime}")
    if(NOT EXISTS "${_selected_runtime}")
        message(FATAL_ERROR "Runtime executable not found: ${_selected_runtime}")
    endif()

    execute_process(
        COMMAND "${_selected_runtime}"
        WORKING_DIRECTORY "${_root}"
        RESULT_VARIABLE _run_result
    )
    if(NOT _run_result EQUAL 0)
        message(FATAL_ERROR "Runtime exited with code ${_run_result}.")
    endif()
endif()
