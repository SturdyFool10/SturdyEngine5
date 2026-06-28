cmake_minimum_required(VERSION 3.28)

set(_valid_view_oses Windows MacOS Linux)
set(_valid_view_profiles Debug RelWithDebInfo Dist)

if(NOT DEFINED STURDY_VIEW_OS)
    message(FATAL_ERROR "STURDY_VIEW_OS is required. Expected one of: ${_valid_view_oses}")
endif()

if(NOT DEFINED STURDY_VIEW_PROFILE)
    message(FATAL_ERROR "STURDY_VIEW_PROFILE is required. Expected one of: ${_valid_view_profiles}")
endif()

if(NOT STURDY_VIEW_OS IN_LIST _valid_view_oses)
    message(FATAL_ERROR "Unsupported STURDY_VIEW_OS='${STURDY_VIEW_OS}'. Expected one of: ${_valid_view_oses}")
endif()

if(NOT STURDY_VIEW_PROFILE IN_LIST _valid_view_profiles)
    message(FATAL_ERROR "Unsupported STURDY_VIEW_PROFILE='${STURDY_VIEW_PROFILE}'. Expected one of: ${_valid_view_profiles}")
endif()

get_filename_component(_zed_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_root_dir "${_zed_dir}/.." ABSOLUTE)

string(TOLOWER "${STURDY_VIEW_OS}" _view_os_lower)
string(TOLOWER "${STURDY_VIEW_PROFILE}" _view_profile_lower)
set(_view_os_dir "${_view_os_lower}")
# Slang's CMake source glob excludes paths containing /windows/, so keep Windows
# view build directories under /win/ while preserving the user-facing OS name.
if(STURDY_VIEW_OS STREQUAL "Windows")
    set(_view_os_dir win)
endif()
set(_preset "view-${_view_os_lower}-${_view_profile_lower}")
set(_compilation_database "build/view/${_view_os_dir}/${_view_profile_lower}")

message(STATUS "Configuring ${STURDY_VIEW_OS} ${STURDY_VIEW_PROFILE} clangd view via preset '${_preset}'")
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
file(APPEND "${_clangd_path}" "# Managed by .zed/view_as_profile.cmake. Use a .zed View As task to switch\n")
file(APPEND "${_clangd_path}" "# this file between OS/profile compile databases. The active view is:\n")
file(APPEND "${_clangd_path}" "#   OS: ${STURDY_VIEW_OS}\n")
file(APPEND "${_clangd_path}" "#   Profile: ${STURDY_VIEW_PROFILE}\n")
file(APPEND "${_clangd_path}" "#\n")
file(APPEND "${_clangd_path}" "# Flags that are NOT settable here (they are clangd command-line options, not\n")
file(APPEND "${_clangd_path}" "# per-TU compile flags) live in the editor config — see .zed/settings.json:\n")
file(APPEND "${_clangd_path}" "#   --experimental-modules-support   clangd builds its own module BMIs\n")
file(APPEND "${_clangd_path}" "#   --header-insertion=never         never auto-#include in a modules-first tree\n")
file(APPEND "${_clangd_path}" "\n")
file(APPEND "${_clangd_path}" "CompileFlags:\n")
file(APPEND "${_clangd_path}" "  # Compilation database for the active OS/profile view (relative to repo root).\n")
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
        message(STATUS "Requested clangd restart with taskkill; Zed will respawn clangd on demand")
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
        message(STATUS "Restarted ${_restarted_count} clangd process(es); Zed will respawn clangd on demand")
    else()
        message(STATUS "No running clangd process was found to restart")
    endif()
endif()

message(STATUS "Active clangd view: ${STURDY_VIEW_OS} ${STURDY_VIEW_PROFILE} (${_compilation_database})")
