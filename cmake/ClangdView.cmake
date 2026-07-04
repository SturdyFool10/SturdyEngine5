# Points clangd at the compile database for a chosen matrix combination — in particular a target
# OS other than the host — so code intelligence can be checked "as if" building for that platform.
# Configures the matching single-config preset (which exports compile_commands.json) and rewrites
# the repo-root .clangd to reference it. Preset name and build tree come from cmake/SturdyMatrix.cmake.
#
#   cmake [-DSTURDY_ARCH=<arch>] -DSTURDY_OS=<os> [-DSTURDY_PROFILE=<profile>] \
#         -P cmake/ClangdView.cmake
#
# STURDY_ARCH defaults to the host, STURDY_PROFILE to Debug. STURDY_OS is required (choosing the
# platform source view is the whole point). Editors that consume CMakePresets.json directly do not
# need this — they just select the desired preset.
cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/SturdyMatrix.cmake")
get_filename_component(_root_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

sturdy_detect_host_arch(_host_arch)

if(NOT DEFINED STURDY_ARCH OR STURDY_ARCH STREQUAL "")
    set(STURDY_ARCH "${_host_arch}")
endif()
if(NOT DEFINED STURDY_PROFILE OR STURDY_PROFILE STREQUAL "")
    set(STURDY_PROFILE "Debug")
endif()
if(NOT DEFINED STURDY_OS OR STURDY_OS STREQUAL "")
    message(FATAL_ERROR "STURDY_OS is required. Expected one of: ${STURDY_OS_LIST}")
endif()

if(NOT STURDY_ARCH IN_LIST STURDY_ARCH_LIST)
    message(FATAL_ERROR "Unsupported STURDY_ARCH='${STURDY_ARCH}'. Expected one of: ${STURDY_ARCH_LIST}")
endif()
if(NOT STURDY_OS IN_LIST STURDY_OS_LIST)
    message(FATAL_ERROR "Unsupported STURDY_OS='${STURDY_OS}'. Expected one of: ${STURDY_OS_LIST}")
endif()
if(NOT STURDY_PROFILE IN_LIST STURDY_PROFILE_LIST)
    message(FATAL_ERROR "Unsupported STURDY_PROFILE='${STURDY_PROFILE}'. Expected one of: ${STURDY_PROFILE_LIST}")
endif()

sturdy_preset_name("${STURDY_ARCH}" "${STURDY_OS}" "${STURDY_PROFILE}" _preset)
sturdy_binary_dir("${STURDY_ARCH}" "${STURDY_OS}" "${STURDY_PROFILE}" _compilation_database)

message(STATUS "Configuring ${STURDY_ARCH} ${STURDY_OS} ${STURDY_PROFILE} clangd view via preset '${_preset}'")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --preset "${_preset}"
    WORKING_DIRECTORY "${_root_dir}"
    RESULT_VARIABLE _configure_result
)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR "Failed to configure preset '${_preset}'")
endif()

set(_clangd_path "${_root_dir}/.clangd")
file(WRITE "${_clangd_path}" "# clangd configuration for SturdyEngine5 (C++23 modules).\n")
file(APPEND "${_clangd_path}" "#\n")
file(APPEND "${_clangd_path}" "# Managed by cmake/ClangdView.cmake. Use a 'View As' editor task (or run the script\n")
file(APPEND "${_clangd_path}" "# directly) to switch this file between arch/OS/profile compile databases. Active view:\n")
file(APPEND "${_clangd_path}" "#   Arch: ${STURDY_ARCH}\n")
file(APPEND "${_clangd_path}" "#   OS: ${STURDY_OS}\n")
file(APPEND "${_clangd_path}" "#   Profile: ${STURDY_PROFILE}\n")
file(APPEND "${_clangd_path}" "#\n")
file(APPEND "${_clangd_path}" "# Flags that are NOT settable here (they are clangd command-line options, not\n")
file(APPEND "${_clangd_path}" "# per-TU compile flags) live in the editor config — see .zed/settings.json:\n")
file(APPEND "${_clangd_path}" "#   --experimental-modules-support   clangd builds its own module BMIs\n")
file(APPEND "${_clangd_path}" "#   --header-insertion=never         never auto-#include in a modules-first tree\n")
file(APPEND "${_clangd_path}" "\n")
file(APPEND "${_clangd_path}" "CompileFlags:\n")
file(APPEND "${_clangd_path}" "  # Compilation database for the active arch/OS/profile view (relative to repo root).\n")
file(APPEND "${_clangd_path}" "  # CompilationDatabase is a CompileFlags sub-key; a top-level key is ignored.\n")
file(APPEND "${_clangd_path}" "  CompilationDatabase: ${_compilation_database}\n")
file(APPEND "${_clangd_path}" "  # clangd builds and manages its own module BMIs. Strip the build system's BMI\n")
file(APPEND "${_clangd_path}" "  # wiring so clangd never (a) tries to load CMake's .pcm files, which are not\n")
file(APPEND "${_clangd_path}" "  # interchangeable with clangd's and make it crash on load, or (b) writes its\n")
file(APPEND "${_clangd_path}" "  # own BMIs back into the CMake build tree. With --experimental-modules-support\n")
file(APPEND "${_clangd_path}" "  # clangd resolves imports by scanning sources itself; without it, this simply\n")
file(APPEND "${_clangd_path}" "  # degrades to plain diagnostics instead of a hard crash.\n")
file(APPEND "${_clangd_path}" "  Remove:\n")
file(APPEND "${_clangd_path}" "    - -fmodule-output=*\n")
file(APPEND "${_clangd_path}" "    - -fmodule-file=*\n")
file(APPEND "${_clangd_path}" "\n")
file(APPEND "${_clangd_path}" "---\n")
file(APPEND "${_clangd_path}" "# C++23 fallback for C++ sources that have no compile_commands.json entry.\n")
file(APPEND "${_clangd_path}" "# Scoped to C++ source extensions so it is never appended to C sources\n")
file(APPEND "${_clangd_path}" "# (e.g. SDL's .c files under _deps), where clang rejects -std=c++23.\n")
file(APPEND "${_clangd_path}" "If:\n")
file(APPEND "${_clangd_path}" "  PathMatch: '.*\\.(cppm|cpp|cxx|cc|ixx)$'\n")
file(APPEND "${_clangd_path}" "CompileFlags:\n")
file(APPEND "${_clangd_path}" "  Add:\n")
file(APPEND "${_clangd_path}" "    - -std=c++23\n")
file(APPEND "${_clangd_path}" "\n")
file(APPEND "${_clangd_path}" "---\n")
file(APPEND "${_clangd_path}" "# Vendored third-party trees (FetchContent's _deps): never background-index or\n")
file(APPEND "${_clangd_path}" "# diagnose them. They are still usable as headers via the -I flags; this only\n")
file(APPEND "${_clangd_path}" "# stops clangd from crawling thousands of dependency TUs into its index.\n")
file(APPEND "${_clangd_path}" "If:\n")
file(APPEND "${_clangd_path}" "  PathMatch: .*/_deps/.*\n")
file(APPEND "${_clangd_path}" "Index:\n")
file(APPEND "${_clangd_path}" "  Background: Skip\n")
file(APPEND "${_clangd_path}" "Diagnostics:\n")
file(APPEND "${_clangd_path}" "  Suppress: \"*\"\n")

set(_root_compile_commands "${_root_dir}/compile_commands.json")
if(EXISTS "${_root_compile_commands}" OR IS_SYMLINK "${_root_compile_commands}")
    file(REMOVE "${_root_compile_commands}")
    message(STATUS "Removed root compile_commands.json; clangd now uses ${_compilation_database}")
endif()

set(_restarted_count 0)
if(CMAKE_HOST_WIN32)
    execute_process(
        COMMAND taskkill /F /IM clangd.exe
        RESULT_VARIABLE _taskkill_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(_taskkill_result EQUAL 0)
        set(_restarted_count 1)
        message(STATUS "Requested clangd restart with taskkill; the editor will respawn clangd on demand")
    else()
        message(STATUS "No running clangd.exe process was found to restart")
    endif()
else()
    if(EXISTS "/proc")
        execute_process(
            COMMAND sh -c "root=$1; count=0; for d in /proc/[0-9]*; do [ -r \"$d/comm\" ] || continue; comm=$(cat \"$d/comm\" 2>/dev/null || true); case \"$comm\" in *clangd*) ;; *) continue ;; esac; cwd=$(readlink \"$d/cwd\" 2>/dev/null || true); if [ \"$cwd\" = \"$root\" ]; then pid=$(basename \"$d\"); if kill -TERM \"$pid\" 2>/dev/null; then count=$((count + 1)); fi; fi; done; printf '%s' \"$count\"" sh "${_root_dir}"
            OUTPUT_VARIABLE _kill_count
            ERROR_QUIET
        )
        string(STRIP "${_kill_count}" _kill_count)
        if(_kill_count MATCHES "^[0-9]+$")
            set(_restarted_count "${_kill_count}")
        endif()
    elseif(CMAKE_HOST_UNIX)
        execute_process(
            COMMAND pkill -TERM -f clangd
            RESULT_VARIABLE _pkill_result
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(_pkill_result EQUAL 0)
            set(_restarted_count 1)
        endif()
    endif()

    if(_restarted_count GREATER 0)
        message(STATUS "Restarted ${_restarted_count} clangd process(es); the editor will respawn clangd on demand")
    else()
        message(STATUS "No running clangd process was found to restart")
    endif()
endif()

message(STATUS "Active clangd view: ${STURDY_ARCH} ${STURDY_OS} ${STURDY_PROFILE} (${_compilation_database})")
