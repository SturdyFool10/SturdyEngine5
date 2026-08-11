include_guard(GLOBAL)

function(sturdy_add_package package_name)
  set(options EXECUTABLE)
  set(one_value_args ARCHIVE_TIMEOUT_SECONDS)
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

        # Match _exclude_relative as a whole run of path segments occurring anywhere in
        # _source_relative (at "/" or string boundaries) — not just as a prefix from the package
        # root. Sources commonly live nested (e.g. src/Platform/Linux/...), so a bare directory
        # name like "Linux" must still exclude everything under it regardless of nesting depth.
        string(REGEX REPLACE "([.^$+*?()\\[\\]{}|\\\\])" "\\\\\\1" _exclude_escaped
          "${_exclude_relative}"
        )
        if(_source_relative MATCHES "(^|/)${_exclude_escaped}(/|$)")
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
  set(_sturdy_named_module_sources)
  foreach(_module IN LISTS _auto_modules)
    file(READ "${_module}" _module_contents LIMIT 4096)
    if(_module_contents MATCHES
        "(^|[\r\n])[ \t]*export[ \t]+module[ \t]+")
      list(APPEND _auto_module_interfaces "${_module}")
      list(APPEND _sturdy_named_module_sources "${_module}")
    else()
      list(APPEND _auto_module_implementations "${_module}")
      if(_module_contents MATCHES
          "(^|[\r\n])[ \t]*module[ \t]+[A-Za-z_][A-Za-z0-9_.:]*[ \t]*;")
        list(APPEND _sturdy_named_module_sources "${_module}")
      endif()
    endif()
  endforeach()

  foreach(_source IN LISTS _auto_sources STURDY_PACKAGE_SOURCES)
    if(_source MATCHES "\\.(cc|cpp|cxx)$")
      file(READ "${_source}" _source_contents LIMIT 4096)
      if(_source_contents MATCHES
          "(^|[\r\n])[ \t]*module[ \t]+[A-Za-z_][A-Za-z0-9_.:]*[ \t]*;")
        list(APPEND _sturdy_named_module_sources "${_source}")
      endif()
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _sturdy_named_module_sources)

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

    if(STURDY_PACKAGE_ARCHIVE_TIMEOUT_SECONDS)
      if(NOT STURDY_PACKAGE_ARCHIVE_TIMEOUT_SECONDS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
          "${package_name}: ARCHIVE_TIMEOUT_SECONDS must be a positive integer or omitted."
        )
      endif()
      if(STURDY_PACKAGE_EXECUTABLE OR STURDY_BUILD_SHARED_LIBS)
        message(FATAL_ERROR
          "${package_name}: ARCHIVE_TIMEOUT_SECONDS is only supported for static libraries."
        )
      endif()

      # CMake has no public target-level archive launcher. RULE_LAUNCH_LINK is the only Ninja
      # hook that wraps each archive subcommand without changing archive rules for dependencies.
      if(CMAKE_HOST_WIN32 AND STURDY_OS STREQUAL "Windows")
        set(_sturdy_archive_timeout_script "${CMAKE_SOURCE_DIR}/cmake/RunWithTimeout.ps1")
        set_property(TARGET "${package_name}" PROPERTY RULE_LAUNCH_LINK
          "powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"${_sturdy_archive_timeout_script}\" -TimeoutSeconds ${STURDY_PACKAGE_ARCHIVE_TIMEOUT_SECONDS}"
        )
      elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND STURDY_OS STREQUAL "Linux")
        find_program(_sturdy_archive_timeout_executable NAMES timeout NO_CACHE)
        if(NOT _sturdy_archive_timeout_executable)
          message(FATAL_ERROR
            "${package_name}: ARCHIVE_TIMEOUT_SECONDS on Linux requires the 'timeout' utility (normally provided by GNU coreutils)."
          )
        endif()
        set_property(TARGET "${package_name}" PROPERTY RULE_LAUNCH_LINK
          "\"${_sturdy_archive_timeout_executable}\" --kill-after=5s ${STURDY_PACKAGE_ARCHIVE_TIMEOUT_SECONDS}s"
        )
      else()
        message(FATAL_ERROR
          "${package_name}: ARCHIVE_TIMEOUT_SECONDS is only supported for native Windows and Linux static libraries."
        )
      endif()
    endif()

    # Clang ThinLTO currently miscompiles some C++20 named-module TUs in Dist: objects that cross
    # module boundaries can corrupt the process heap before the first spdlog allocation. Keep IPO/LTO
    # enabled globally, but compile named-module translation units as native objects so LTO still covers
    # ordinary .cpp/.cxx files and third-party code.
    if(STURDY_ENABLE_LTO AND CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND _sturdy_named_module_sources)
      set_source_files_properties(${_sturdy_named_module_sources}
        PROPERTIES
          COMPILE_OPTIONS "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:Dist>>:-fno-lto>"
      )
    endif()

    # thirdparty/licenses/ is regenerated (and gitignored), so make sure it's populated before
    # this target's compiler invocations rather than only when CMake happens to reconfigure.
    add_dependencies("${package_name}" sturdy_collect_licenses)

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
                # Some packages colocate headers and .cpp under src/<Package>/... instead of the
                # separate Include/<Package>/... tree (both conventions currently coexist across
                # the codebase) - add it too so #include <Package/Foo.hpp> resolves either way.
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
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
                "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
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
  sturdy_enable_static_dead_stripping("${package_name}")

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

    # DirectStorage ships as two DLLs (dstorage.dll + its own dstoragecore.dll dependency) that
    # must sit next to any executable that transitively links Sturdy::Core on Windows -- see
    # sturdy_fetch_directstorage()'s own doc comment for why both are required. Centralized here
    # (rather than in every product's own CMakeLists.txt) so it automatically covers every demo/
    # test executable this function builds, present or future.
    if(STURDY_OS STREQUAL "Windows" AND TARGET Sturdy::DirectStorage)
      get_target_property(_sturdy_dstorage_dll Sturdy::DirectStorage STURDY_DIRECTSTORAGE_DLL)
      get_target_property(_sturdy_dstorage_core_dll Sturdy::DirectStorage STURDY_DIRECTSTORAGE_CORE_DLL)
      if(_sturdy_dstorage_dll AND _sturdy_dstorage_core_dll)
        add_custom_command(TARGET "${package_name}" POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                  "${_sturdy_dstorage_dll}"
                  "${_sturdy_dstorage_core_dll}"
                  "$<TARGET_FILE_DIR:${package_name}>"
                VERBATIM
            )
      endif()
    endif()

    # Generic runtime-DLL copy for every SHARED library actually in this target's link graph. Not
    # needed for Slang today -- sturdy_fetch_slang() forces SLANG_LIB_TYPE=STATIC precisely so no
    # slang-compiler.dll exists to copy in the first place -- but kept as a general safety net for
    # any dependency that does produce a real CMake SHARED target (unlike the DirectStorage copy
    # above, a bare extracted NuGet .lib/.dll pair CMake has no target-level knowledge of): the
    # generic $<TARGET_RUNTIME_DLLS:...> generator expression (CMake's own recommended mechanism
    # for exactly this) finds those without hardcoding any dependency-internal target names.
    #
    # $<TARGET_RUNTIME_DLLS:...> is empty on an all-static build like this one's default, and
    # `cmake -E copy_if_different <files...> <dest>` errors with no <files...> at all -- the
    # $<IF:...> below swaps in the `true` no-op subcommand for that case (which, unlike
    # copy_if_different, tolerates and ignores the dangling <dest> argument) rather than skipping
    # the whole COMMAND, since COMMAND_EXPAND_LISTS does not drop an empty-list COMMAND for us.
    add_custom_command(TARGET "${package_name}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E "$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${package_name}>>,copy_if_different,true>"
              "$<TARGET_RUNTIME_DLLS:${package_name}>"
              "$<TARGET_FILE_DIR:${package_name}>"
            COMMAND_EXPAND_LISTS
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

function(sturdy_enable_static_dead_stripping target_name)
  if(NOT STURDY_ENABLE_STATIC_DEAD_STRIPPING OR STURDY_BUILD_SHARED_LIBS)
    return()
  endif()

  get_target_property(_target_type "${target_name}" TYPE)
  if(_target_type STREQUAL "INTERFACE_LIBRARY" OR
     _target_type STREQUAL "SHARED_LIBRARY" OR
     _target_type STREQUAL "MODULE_LIBRARY")
    return()
  endif()

  if(MSVC)
    set(_sturdy_section_options /Gy /Gw)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(_sturdy_section_options -ffunction-sections -fdata-sections)
  else()
    message(WARNING
      "Static dead stripping is enabled, but ${CMAKE_CXX_COMPILER_ID} "
      "has no configured section-emission policy."
    )
    return()
  endif()

  # The dead-stripping *linker* flag depends on which linker actually runs, not which compiler
  # frontend invoked it: Clang targeting Windows still drives lld-link (a COFF linker), not a
  # GNU-style one, so it needs the MSVC spelling even though CMAKE_CXX_COMPILER_ID reads "Clang"
  # there (that mismatch is what previously sent lld-link an unrecognized "--gc-sections").
  if(STURDY_OS STREQUAL "Windows")
    set(_sturdy_dead_strip_link_option "LINKER:/OPT:REF")
  elseif(STURDY_OS STREQUAL "MacOS")
    set(_sturdy_dead_strip_link_option "LINKER:-dead_strip")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(_sturdy_dead_strip_link_option "LINKER:--gc-sections")
  else()
    message(WARNING
      "Static dead stripping is enabled, but ${CMAKE_CXX_COMPILER_ID} on ${STURDY_OS} "
      "has no configured linker policy."
    )
    return()
  endif()

  target_compile_options("${target_name}" PRIVATE ${_sturdy_section_options})
  if(_target_type STREQUAL "EXECUTABLE")
    target_link_options(
      "${target_name}"
      PRIVATE "${_sturdy_dead_strip_link_option}"
    )
  else()
    # Static archives do not perform a link step. Propagate section GC to
    # the final consumer instead of forcing whole-archive semantics.
    target_link_options(
      "${target_name}"
      INTERFACE "${_sturdy_dead_strip_link_option}"
    )
  endif()
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
