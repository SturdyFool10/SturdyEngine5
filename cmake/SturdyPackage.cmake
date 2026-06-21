include_guard(GLOBAL)

function(sturdy_add_package package_name)
    set(options EXECUTABLE)
    set(one_value_args)
    set(multi_value_args
        SOURCES
        MODULES
        PUBLIC_DEPS
        PRIVATE_DEPS
        DEBUG_PUBLIC_DEPS
        DEBUG_PRIVATE_DEPS
        PUBLIC_DEFINES
        PRIVATE_DEFINES
    )
    cmake_parse_arguments(STURDY_PACKAGE
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(STURDY_PACKAGE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown sturdy_add_package arguments for ${package_name}: ${STURDY_PACKAGE_UNPARSED_ARGUMENTS}")
    endif()

    file(GLOB _root_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hxx"
    )
    file(GLOB_RECURSE _all_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.hxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Include/*.hxx"
    )
    file(GLOB_RECURSE _all_modules CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.ixx"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
    )
    list(APPEND _all_sources ${_root_sources} ${STURDY_PACKAGE_SOURCES})
    list(APPEND _all_modules ${STURDY_PACKAGE_MODULES})

    set(_compile_sources)
    foreach(_source IN LISTS _all_sources)
        if(_source MATCHES "\\.(c|cc|cpp|cxx)$")
            list(APPEND _compile_sources "${_source}")
        endif()
    endforeach()

    if(_compile_sources OR _all_modules)
        if(STURDY_PACKAGE_EXECUTABLE)
            add_executable("${package_name}" ${_all_sources})
        elseif(STURDY_BUILD_SHARED_LIBS)
            add_library("${package_name}" SHARED ${_all_sources})
        else()
            add_library("${package_name}" STATIC ${_all_sources})
        endif()

        set_target_properties("${package_name}" PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            CXX_SCAN_FOR_MODULES ON
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )

        if(_all_modules)
            target_sources("${package_name}"
                PUBLIC
                    FILE_SET sturdy_cxx_modules TYPE CXX_MODULES FILES
                        ${_all_modules}
            )
        endif()

        target_include_directories("${package_name}"
            PUBLIC
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>"
                "$<INSTALL_INTERFACE:include>"
            PRIVATE
                "${CMAKE_CURRENT_SOURCE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/Source"
        )
    else()
        add_library("${package_name}" INTERFACE)
        target_include_directories("${package_name}"
            INTERFACE
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>"
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>"
                "$<INSTALL_INTERFACE:include>"
        )
        message(STATUS "${package_name}: no compile sources found; creating an INTERFACE target for now.")
    endif()

    if(NOT (STURDY_PACKAGE_EXECUTABLE AND _compile_sources))
        add_library("Sturdy::${package_name}" ALIAS "${package_name}")
    endif()

    sturdy_configure_package_target("${package_name}"
        PUBLIC_DEPS ${STURDY_PACKAGE_PUBLIC_DEPS}
        PRIVATE_DEPS ${STURDY_PACKAGE_PRIVATE_DEPS}
        DEBUG_PUBLIC_DEPS ${STURDY_PACKAGE_DEBUG_PUBLIC_DEPS}
        DEBUG_PRIVATE_DEPS ${STURDY_PACKAGE_DEBUG_PRIVATE_DEPS}
        PUBLIC_DEFINES ${STURDY_PACKAGE_PUBLIC_DEFINES}
        PRIVATE_DEFINES ${STURDY_PACKAGE_PRIVATE_DEFINES}
    )

    if(STURDY_PACKAGE_EXECUTABLE AND _compile_sources)
        set(_sturdy_debuggable_path "${CMAKE_BINARY_DIR}/bin/${package_name}.exe")

        add_custom_command(TARGET "${package_name}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${package_name}>" "${_sturdy_debuggable_path}"
            VERBATIM
        )

        add_custom_target("run_${package_name}"
            COMMAND "$<TARGET_FILE:${package_name}>"
            DEPENDS "${package_name}"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${package_name}>"
            USES_TERMINAL
            VERBATIM
        )
    endif()
endfunction()

function(sturdy_configure_package_target target_name)
    set(options)
    set(one_value_args)
    set(multi_value_args
        PUBLIC_DEPS
        PRIVATE_DEPS
        DEBUG_PUBLIC_DEPS
        DEBUG_PRIVATE_DEPS
        PUBLIC_DEFINES
        PRIVATE_DEFINES
    )
    cmake_parse_arguments(STURDY_TARGET
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    get_target_property(_target_type "${target_name}" TYPE)
    if(_target_type STREQUAL "INTERFACE_LIBRARY")
        set(_public_scope INTERFACE)
        set(_private_scope INTERFACE)
    else()
        set(_public_scope PUBLIC)
        set(_private_scope PRIVATE)

        sturdy_enable_warnings("${target_name}")
    endif()

    target_link_libraries("${target_name}"
        ${_public_scope}
            ${STURDY_TARGET_PUBLIC_DEPS}
        ${_private_scope}
            ${STURDY_TARGET_PRIVATE_DEPS}
    )

    foreach(_debug_dep IN LISTS STURDY_TARGET_DEBUG_PUBLIC_DEPS)
        target_link_libraries("${target_name}" ${_public_scope} "$<$<CONFIG:Debug>:${_debug_dep}>")
    endforeach()

    foreach(_debug_dep IN LISTS STURDY_TARGET_DEBUG_PRIVATE_DEPS)
        target_link_libraries("${target_name}" ${_private_scope} "$<$<CONFIG:Debug>:${_debug_dep}>")
    endforeach()

    target_compile_definitions("${target_name}"
        ${_public_scope}
            ${STURDY_TARGET_PUBLIC_DEFINES}
        ${_private_scope}
            ${STURDY_TARGET_PRIVATE_DEFINES}
    )
endfunction()

function(sturdy_enable_warnings target_name)
    if(MSVC)
        target_compile_options("${target_name}" PRIVATE /W4)
        if(STURDY_WARNINGS_AS_ERRORS)
            target_compile_options("${target_name}" PRIVATE /WX)
        endif()
    else()
        target_compile_options("${target_name}" PRIVATE -Wall -Wextra -Wpedantic)
        if(STURDY_WARNINGS_AS_ERRORS)
            target_compile_options("${target_name}" PRIVATE -Werror)
        endif()
    endif()
endfunction()
