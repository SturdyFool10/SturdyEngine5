cmake_minimum_required(VERSION 3.28)

foreach(_required IN ITEMS STURDY_PROBE STURDY_FORBIDDEN_PATTERN)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required.")
    endif()
endforeach()
if(NOT DEFINED STURDY_PROBE_LABEL)
    set(STURDY_PROBE_LABEL "probe executable")
endif()
if(NOT EXISTS "${STURDY_PROBE}")
    message(FATAL_ERROR "Probe executable does not exist: ${STURDY_PROBE}")
endif()

execute_process(
    COMMAND "${STURDY_PROBE}"
    RESULT_VARIABLE _probe_result
    OUTPUT_VARIABLE _probe_stdout
    ERROR_VARIABLE _probe_stderr
)
if(NOT _probe_result EQUAL 0)
    message(FATAL_ERROR "${STURDY_PROBE_LABEL} exited with ${_probe_result}.")
endif()
if(NOT _probe_stdout STREQUAL "" OR NOT _probe_stderr STREQUAL "")
    message(FATAL_ERROR
        "${STURDY_PROBE_LABEL} must start and exit silently.\n"
        "stdout: ${_probe_stdout}\n"
        "stderr: ${_probe_stderr}"
    )
endif()

string(TOLOWER "${STURDY_FORBIDDEN_PATTERN}" _forbidden_pattern)
find_program(_sturdy_nm NAMES llvm-nm nm)
if(_sturdy_nm)
    execute_process(
        COMMAND "${_sturdy_nm}" -C "${STURDY_PROBE}"
        RESULT_VARIABLE _nm_result
        OUTPUT_VARIABLE _symbols
        ERROR_VARIABLE _nm_error
    )
    if(NOT _nm_result EQUAL 0)
        message(FATAL_ERROR "Failed to inspect ${STURDY_PROBE_LABEL} symbols: ${_nm_error}")
    endif()
    string(TOLOWER "${_symbols}" _symbols_lower)
    if(_symbols_lower MATCHES "${_forbidden_pattern}")
        message(FATAL_ERROR
            "${STURDY_PROBE_LABEL} retained forbidden '${STURDY_FORBIDDEN_PATTERN}' symbols:\n"
            "${_symbols}"
        )
    endif()
endif()

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${STURDY_PROBE}"
    RESOLVED_DEPENDENCIES_VAR _runtime_dependencies
    UNRESOLVED_DEPENDENCIES_VAR _unresolved_dependencies
)
foreach(_dependency IN LISTS _runtime_dependencies _unresolved_dependencies)
    get_filename_component(_dependency_name "${_dependency}" NAME)
    string(TOLOWER "${_dependency_name}" _dependency_lower)
    if(_dependency_lower MATCHES "${_forbidden_pattern}")
        message(FATAL_ERROR
            "${STURDY_PROBE_LABEL} retained forbidden runtime dependency: ${_dependency}"
        )
    endif()
endforeach()

message(STATUS
    "${STURDY_PROBE_LABEL} has no '${STURDY_FORBIDDEN_PATTERN}' binary footprint."
)
