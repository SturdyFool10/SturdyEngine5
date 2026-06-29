cmake_minimum_required(VERSION 3.20)

get_filename_component(_script_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
get_filename_component(_root "${_script_dir}/.." ABSOLUTE)

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(NOT CLANG_FORMAT_EXECUTABLE)
    message(FATAL_ERROR "clang-format was not found in PATH. Install clang-format or launch the editor from a shell where clang-format is available.")
endif()

set(_patterns
    "*.c"
    "*.cc"
    "*.cpp"
    "*.cxx"
    "*.h"
    "*.hh"
    "*.hpp"
    "*.hxx"
    "*.ipp"
    "*.ixx"
    "*.cppm"
    "*.ccm"
    "*.cxxm"
)

set(_files)
foreach(_pattern IN LISTS _patterns)
    file(GLOB_RECURSE _matched "${_root}/${_pattern}")
    list(APPEND _files ${_matched})
endforeach()

list(REMOVE_DUPLICATES _files)

set(_format_files)
foreach(_file IN LISTS _files)
    file(RELATIVE_PATH _relative "${_root}" "${_file}")

    if(_relative MATCHES "^(\\.git|\\.lore|build)(/|$)")
        continue()
    endif()

    list(APPEND _format_files "${_file}")
endforeach()

if(NOT _format_files)
    message(STATUS "No C++ files found to format.")
    return()
endif()

execute_process(
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i --style=file ${_format_files}
    WORKING_DIRECTORY "${_root}"
    RESULT_VARIABLE _result
)

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "clang-format failed with exit code ${_result}.")
endif()

list(LENGTH _format_files _format_file_count)
message(STATUS "Formatted ${_format_file_count} C++ files.")
