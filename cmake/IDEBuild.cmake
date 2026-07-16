# Generic, editor-agnostic build/run bridge. Any editor that cannot consume CMakePresets.json
# directly (Zed, Helix, Vim, plugin-less Neovim, Atom, ...) drives the matrix through this one
# script instead of hard-coding preset names or build paths — those are derived from the axis
# lists in cmake/SturdyMatrix.cmake, so adding an arch/os/profile needs no change here.
#
#   cmake [-DSTURDY_ARCH=<arch>] [-DSTURDY_OS=<os>] [-DSTURDY_COMPILER=<compiler>] \
#         -DSTURDY_PROFILE=<profile> [-DSTURDY_RUN=ON | -DSTURDY_RENDERDOC=ON] \
#         [-DSTURDY_TARGET=<target>] \
#         [-DSTURDY_ALL_PROFILES=ON] -P cmake/IDEBuild.cmake
#
# STURDY_ARCH / STURDY_OS default to the host. STURDY_COMPILER defaults to Clang (GCC is only
# valid for native arch/os — see the STURDY_COMPILER guard in SturdyMatrix.cmake's
# sturdy_apply_matrix(), and Web never has a GCC variant). STURDY_PROFILE defaults to Debug
# (ignored when STURDY_ALL_PROFILES is set, which builds every profile for the selected arch/os).
# STURDY_RUN launches the built target afterwards (single profile, host-native only).
# STURDY_RENDERDOC launches it through renderdoccmd, ready for an F12/Print Screen capture, and
# writes timestamped captures beneath <project-root>/captures (single profile, host-native only).
# STURDY_TARGET defaults to Runtime.
cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/SturdyMatrix.cmake")
get_filename_component(_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# CMakePresets.json is generated (gitignored, not hand-edited — see GeneratePresets.cmake) and
# the --preset invocation below needs it to exist, so regenerate it unconditionally first. This
# is also every editor task's only trigger for that generator, since none of them call it directly.
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
if(NOT DEFINED STURDY_TARGET OR STURDY_TARGET STREQUAL "")
    set(STURDY_TARGET "Runtime")
endif()
if(NOT DEFINED STURDY_COMPILER OR STURDY_COMPILER STREQUAL "")
    set(STURDY_COMPILER "Clang")
endif()

if(STURDY_RUN AND STURDY_RENDERDOC)
    message(FATAL_ERROR "STURDY_RUN and STURDY_RENDERDOC are mutually exclusive launch modes.")
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
if(STURDY_OS STREQUAL "Web" AND NOT STURDY_COMPILER STREQUAL "Clang")
    message(FATAL_ERROR "STURDY_OS=Web has no GCC variant (Emscripten's toolchain is clang-based only).")
endif()

if(STURDY_ALL_PROFILES)
    set(_profiles ${STURDY_PROFILE_LIST})
else()
    if(NOT DEFINED STURDY_PROFILE OR STURDY_PROFILE STREQUAL "")
        set(STURDY_PROFILE "Debug")
    endif()
    if(NOT STURDY_PROFILE IN_LIST STURDY_PROFILE_LIST)
        message(FATAL_ERROR "Unsupported STURDY_PROFILE='${STURDY_PROFILE}'. Expected one of: ${STURDY_PROFILE_LIST}")
    endif()
    set(_profiles "${STURDY_PROFILE}")
endif()

foreach(_profile IN LISTS _profiles)
    sturdy_preset_name("${STURDY_ARCH}" "${STURDY_OS}" "${_profile}" "${STURDY_COMPILER}" _preset)
    sturdy_binary_dir("${STURDY_ARCH}" "${STURDY_OS}" "${_profile}" "${STURDY_COMPILER}" _binary_dir)

    message(STATUS "==> Configuring ${_preset}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --preset "${_preset}"
        WORKING_DIRECTORY "${_root}"
        RESULT_VARIABLE _configure_result
    )
    if(NOT _configure_result EQUAL 0)
        message(FATAL_ERROR "CMake configure failed for preset '${_preset}' (exit ${_configure_result}).")
    endif()

    message(STATUS "==> Building ${STURDY_TARGET} (${_preset})")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_root}/${_binary_dir}" --parallel --target "${STURDY_TARGET}"
        WORKING_DIRECTORY "${_root}"
        RESULT_VARIABLE _build_result
    )
    if(NOT _build_result EQUAL 0)
        message(FATAL_ERROR "CMake build failed for preset '${_preset}' (exit ${_build_result}).")
    endif()
endforeach()

# Stable, arch/os-independent alias so editor debug/launch configs (e.g. .zed/debug.json) can
# reference build/host/<profile>/... instead of baking a contributor's specific arch and OS into
# shared, committed files. Best-effort: symlink creation may fail on Windows without the required
# privilege, in which case such configs fall back to editing the concrete build/<arch>/<os> path.
if(STURDY_ARCH STREQUAL _host_arch AND STURDY_OS STREQUAL _host_os)
    string(TOLOWER "${STURDY_ARCH}" _arch_dir)
    sturdy_os_dir_name("${STURDY_OS}" _os_dir)
    set(_host_link "${_root}/build/host")
    if(IS_SYMLINK "${_host_link}" OR EXISTS "${_host_link}")
        file(REMOVE "${_host_link}")
    endif()
    file(CREATE_LINK "${_root}/build/${_arch_dir}/${_os_dir}" "${_host_link}"
        SYMBOLIC RESULT _link_result)
    if(_link_result EQUAL 0)
        message(STATUS "build/host -> build/${_arch_dir}/${_os_dir}")
    endif()
endif()

if((STURDY_RUN OR STURDY_RENDERDOC) AND NOT STURDY_ALL_PROFILES)
    sturdy_binary_dir("${STURDY_ARCH}" "${STURDY_OS}" "${STURDY_PROFILE}" "${STURDY_COMPILER}" _binary_dir)
    if(CMAKE_HOST_WIN32)
        set(_binary "${_root}/${_binary_dir}/bin/${STURDY_TARGET}.exe")
    else()
        set(_binary "${_root}/${_binary_dir}/bin/${STURDY_TARGET}")
    endif()
    if(NOT EXISTS "${_binary}")
        message(FATAL_ERROR "Target executable not found: ${_binary}")
    endif()

    if(STURDY_RENDERDOC)
        if(NOT STURDY_ARCH STREQUAL _host_arch OR NOT STURDY_OS STREQUAL _host_os)
            message(FATAL_ERROR "RenderDoc capture tasks can only launch a host-native target.")
        endif()
        find_program(_renderdoccmd NAMES renderdoccmd renderdoccmd.exe)
        if(NOT _renderdoccmd)
            message(FATAL_ERROR "renderdoccmd was not found on PATH. Install RenderDoc to use this task.")
        endif()

        set(_capture_directory "${_root}/captures")
        file(MAKE_DIRECTORY "${_capture_directory}")
        string(TIMESTAMP _capture_timestamp "%Y-%m-%d_%H-%M-%S")
        set(_capture_template "${_capture_directory}/${STURDY_TARGET}_${_capture_timestamp}")

        message(STATUS "==> Launching ${_binary} through RenderDoc")
        message(STATUS "==> Press F12 or Print Screen to capture into ${_capture_directory}")
        set(_renderdoc_command)
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            # RenderDoc's Linux package commonly supports Xlib/XCB capture but not
            # VK_KHR_wayland_surface. Force SDL through XWayland so Vulkan injection works on
            # both X11 desktops and Wayland desktops with XWayland available.
            list(APPEND _renderdoc_command "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=x11)
        endif()
        list(APPEND _renderdoc_command
            "${_renderdoccmd}" capture
            --wait-for-exit
            --working-dir "${_root}"
            --capture-file "${_capture_template}"
            "${_binary}"
        )
        execute_process(
            COMMAND ${_renderdoc_command}
            WORKING_DIRECTORY "${_root}"
            RESULT_VARIABLE _capture_result
        )
        if(NOT _capture_result EQUAL 0)
            message(FATAL_ERROR "RenderDoc capture session exited with code ${_capture_result}.")
        endif()
    else()
        message(STATUS "==> Running ${_binary}")
        execute_process(
            COMMAND "${_binary}"
            WORKING_DIRECTORY "${_root}"
            RESULT_VARIABLE _run_result
        )
        if(NOT _run_result EQUAL 0)
            message(FATAL_ERROR "${STURDY_TARGET} exited with code ${_run_result}.")
        endif()
    endif()
endif()
