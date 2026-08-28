cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED STURDY_STATIC_PROBE)
    message(FATAL_ERROR "STURDY_STATIC_PROBE must name the probe executable.")
endif()
if(NOT DEFINED STURDY_STATIC_AGGREGATE_LABEL)
    set(STURDY_STATIC_AGGREGATE_LABEL "Sturdy::Engine")
endif()
if(NOT EXISTS "${STURDY_STATIC_PROBE}")
    message(FATAL_ERROR
        "Static aggregate probe does not exist: ${STURDY_STATIC_PROBE}"
    )
endif()

execute_process(
    COMMAND "${STURDY_STATIC_PROBE}"
    RESULT_VARIABLE _probe_result
    OUTPUT_VARIABLE _probe_stdout
    ERROR_VARIABLE _probe_stderr
)
if(NOT _probe_result EQUAL 0)
    message(FATAL_ERROR "Static aggregate probe exited with ${_probe_result}.")
endif()
if(NOT _probe_stdout STREQUAL "" OR NOT _probe_stderr STREQUAL "")
    message(FATAL_ERROR
        "An unused static aggregate must start and exit silently.\n"
        "stdout: ${_probe_stdout}\n"
        "stderr: ${_probe_stderr}"
    )
endif()

find_program(_sturdy_nm NAMES llvm-nm nm)
if(_sturdy_nm)
    execute_process(
        COMMAND "${_sturdy_nm}" -C --defined-only "${STURDY_STATIC_PROBE}"
        RESULT_VARIABLE _nm_result
        OUTPUT_VARIABLE _defined_symbols
        ERROR_VARIABLE _nm_error
    )
    if(NOT _nm_result EQUAL 0)
        message(FATAL_ERROR "Failed to inspect probe symbols: ${_nm_error}")
    endif()

    string(TOLOWER "${_defined_symbols}" _defined_symbols_lower)
    string(FIND "${_defined_symbols_lower}" "sft::" _sturdy_symbol)
    if(NOT _sturdy_symbol EQUAL -1)
        message(FATAL_ERROR
            "Linking but not using ${STURDY_STATIC_AGGREGATE_LABEL} retained Sturdy symbols:\n"
            "${_defined_symbols}"
        )
    endif()
endif()

# Dominates this script's runtime: it walks the probe's whole transitive DLL graph, which costs
# several seconds even for a trivial binary (measured at ~8s for notepad.exe on a dev machine). The
# tests that run this therefore need a timeout in the tens of seconds, not ten.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${STURDY_STATIC_PROBE}"
    RESOLVED_DEPENDENCIES_VAR _runtime_dependencies
    UNRESOLVED_DEPENDENCIES_VAR _unresolved_dependencies
)

set(_forbidden_runtime_dependency_pattern
    "vulkan|sdl|glfw|slang|freetype|harfbuzz|mimalloc|box3d|lunasvg"
)
foreach(_dependency IN LISTS _runtime_dependencies _unresolved_dependencies)
    string(TOLOWER "${_dependency}" _dependency_lower)
    if(_dependency_lower MATCHES "${_forbidden_runtime_dependency_pattern}")
        message(FATAL_ERROR
            "Unused static aggregate retained subsystem runtime dependency: "
            "${_dependency}"
        )
    endif()
endforeach()

message(STATUS
    "Unused ${STURDY_STATIC_AGGREGATE_LABEL} static aggregate has no subsystem runtime footprint."
)
