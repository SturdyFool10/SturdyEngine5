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
        EXCLUDE_SOURCE_DIRS
    )
  cmake_parse_arguments(STURDY_PACKAGE
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

  if(STURDY_PACKAGE_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "Unknown sturdy_add_package arguments for ${package_name}: "
      "${STURDY_PACKAGE_UNPARSED_ARGUMENTS}"
    )
  endif()

  set(_sturdy_glob_options)
  if(STURDY_GLOB_CONFIGURE_DEPENDS)
    list(APPEND _sturdy_glob_options CONFIGURE_DEPENDS)
  endif()

  set(_sturdy_source_globs
        "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
    )
  if(STURDY_INCLUDE_HEADERS_IN_TARGETS)
    list(APPEND _sturdy_source_globs
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hxx"
    )
  endif()

  file(GLOB_RECURSE _auto_sources ${_sturdy_glob_options}
        ${_sturdy_source_globs}
    )
  file(GLOB_RECURSE _auto_modules ${_sturdy_glob_options}
        "${CMAKE_CURRENT_SOURCE_DIR}/*.ixx"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cppm"
    )

  foreach(_source_list _auto_sources _auto_modules)
    set(_filtered_sources)
    foreach(_source IN LISTS ${_source_list})
      file(RELATIVE_PATH _source_relative
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${_source}"
      )
      file(TO_CMAKE_PATH "${_source_relative}" _source_relative)

      set(_excluded FALSE)
      foreach(_exclude_dir IN LISTS STURDY_PACKAGE_EXCLUDE_SOURCE_DIRS)
        if(IS_ABSOLUTE "${_exclude_dir}")
          file(RELATIVE_PATH _exclude_relative
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${_exclude_dir}"
          )
        else()
          set(_exclude_relative "${_exclude_dir}")
        endif()

        file(TO_CMAKE_PATH "${_exclude_relative}" _exclude_relative)
        string(REGEX REPLACE "^\\./" "" _exclude_relative
          "${_exclude_relative}"
        )
        string(REGEX REPLACE "/$" "" _exclude_relative
          "${_exclude_relative}"
        )
        if(_exclude_relative STREQUAL "")
          continue()
        endif()

        set(_exclude_prefix "${_exclude_relative}/")
        string(FIND "${_source_relative}" "${_exclude_prefix}"
          _exclude_prefix_index
        )
        if(_source_relative STREQUAL _exclude_relative OR
            _exclude_prefix_index EQUAL 0)
          set(_excluded TRUE)
          break()
        endif()
      endforeach()

      if(NOT _excluded)
        list(APPEND _filtered_sources "${_source}")
      endif()
    endforeach()
    set(${_source_list} ${_filtered_sources})
  endforeach()

  set(_auto_module_interfaces)
  set(_auto_module_implementations)
  foreach(_module IN LISTS _auto_modules)
    file(READ "${_module}" _module_contents LIMIT 4096)
    if(_module_contents MATCHES
        "(^|[\r\n])[ \t]*export[ \t]+module[ \t]+")
      list(APPEND _auto_module_interfaces "${_module}")
    else()
      list(APPEND _auto_module_implementations "${_module}")
    endif()
  endforeach()

  set(_all_sources
    ${_auto_sources}
    ${_auto_module_implementations}
    ${STURDY_PACKAGE_SOURCES}
  )
  set(_all_modules ${_auto_module_interfaces} ${STURDY_PACKAGE_MODULES})
  list(REMOVE_DUPLICATES _all_sources)
  list(REMOVE_DUPLICATES _all_modules)

  set(_compile_sources)
  foreach(_source IN LISTS _all_sources)
    if(_source MATCHES "\\.(c|cc|cpp|cxx|cppm|ixx)$")
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
            OPTIMIZE_DEPENDENCIES ON
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
    message(STATUS
      "${package_name}: no compile sources found; "
      "creating an INTERFACE target for now."
    )
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
    set(_sturdy_debuggable_path
      "${CMAKE_BINARY_DIR}/bin/${package_name}.exe"
    )

    add_custom_command(TARGET "${package_name}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${CMAKE_BINARY_DIR}/bin"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "$<TARGET_FILE:${package_name}>"
              "${_sturdy_debuggable_path}"
            VERBATIM
        )

    set_target_properties("${package_name}" PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            XCODE_SCHEME_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )

    add_custom_target("run_${package_name}"
            COMMAND "$<TARGET_FILE:${package_name}>"
            DEPENDS "${package_name}"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
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
    target_link_libraries("${target_name}"
      ${_public_scope}
        "$<$<CONFIG:Debug>:${_debug_dep}>"
    )
  endforeach()

  foreach(_debug_dep IN LISTS STURDY_TARGET_DEBUG_PRIVATE_DEPS)
    target_link_libraries("${target_name}"
      ${_private_scope}
        "$<$<CONFIG:Debug>:${_debug_dep}>"
    )
  endforeach()

  target_compile_definitions("${target_name}"
        ${_public_scope}
            ${STURDY_TARGET_PUBLIC_DEFINES}
            "$<$<CONFIG:Debug>:DEBUG>"
            # DIST marks a shipping build (Release-optimized, no console). It is defined ONLY for the
            # Dist configuration — a plain Release build is optimized but keeps DIST undefined, so it
            # still gets the console main(). On Windows this gates the WinMain entry point.
            "$<$<CONFIG:Dist>:DIST>"
            # Forces the Windows GUI (WinMain) entry point regardless of configuration.
            "$<$<BOOL:${SFT_USE_WINMAIN}>:SFT_USE_WINMAIN>"
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
