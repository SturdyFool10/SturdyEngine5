cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED STURDY_SOURCE_DIR)
    message(FATAL_ERROR
        "STURDY_SOURCE_DIR must point at the SturdyEngine source root."
    )
endif()

set(_sturdy_base_dependency_files
    "${STURDY_SOURCE_DIR}/Engine/CMakeLists.txt"
    "${STURDY_SOURCE_DIR}/cmake/SturdyDependencies.cmake"
)

foreach(_dependency_file IN LISTS _sturdy_base_dependency_files)
    if(NOT EXISTS "${_dependency_file}")
        message(FATAL_ERROR
            "Base dependency boundary input does not exist:\n"
            "  ${_dependency_file}"
        )
    endif()

    file(READ "${_dependency_file}" _dependency_contents)
    string(TOLOWER "${_dependency_contents}" _dependency_contents_lower)
    string(FIND "${_dependency_contents_lower}" "box3d" _box3d_reference)

    if(NOT _box3d_reference EQUAL -1)
        message(FATAL_ERROR
            "The base engine dependency graph references Box3D in "
            "${_dependency_file}. Physics dependencies must live behind a "
            "separately selected physics adapter."
        )
    endif()
endforeach()

message(STATUS
    "Base engine dependency graph does not reference Box3D."
)
