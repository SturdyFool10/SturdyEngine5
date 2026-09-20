include_guard(GLOBAL)

# Test registration. Tests are found by wildcard: every tests/**/*Test.cpp (or *Test.c) under a package
# becomes an executable linked to that package, named <Package><Path><Stem> (the package prefix is not
# repeated when the file already starts with it; directories are folded in, so Text/DocumentTest.cpp in
# Renderer is RendererTextDocumentTest) and registered with CTest. Adding a test is adding the file.
# Files that do not end in "Test" (fixtures, probes, helpers) are never picked up.
#
#   sturdy_add_tests(<package> [TIMEOUT <s>] [TIMEOUTS <key>=<s>...] [NAMES <key>=<name>...]
#                              [KEEP_ASSERTS <key>...] [BIN_DIR <key>...] [EXCLUDE <key>...])
#
# where <key> is a test's path under tests/ without its extension (e.g. Text/DocumentTest). Anything
# a key cannot express is registered by hand with sturdy_add_test() after EXCLUDE-ing it.
#
# Everything here is a no-op unless tests can be built and run in this tree, so callers need no guard.

# Tests are skipped when cross-compiling (including Emscripten, which cannot run them under CTest).
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
  set(STURDY_TESTS_ENABLED ON)
else()
  set(STURDY_TESTS_ENABLED OFF)
endif()

set(STURDY_DEFAULT_TEST_TIMEOUT 10 CACHE INTERNAL "Seconds a test may run unless it says otherwise.")

# Registers one test executable and its CTest entry.
#   sturdy_add_test(<package> <source> [NAME <name>] [TIMEOUT <s>] [C_STANDARD <n>] [ARGS <arg>...]
#                   [DEFINES <def>...] [LINK <lib>...] [DEPENDS <target>...]
#                   [KEEP_ASSERTS] [BIN_DIR] [NO_TEST])
# KEEP_ASSERTS: the test states its expectations with assert(), which NDEBUG (Release/Dist) compiles
#   to nothing so the test "passes" without checking anything. -UNDEBUG comes after the configuration's
#   own flags and keeps them live.
# BIN_DIR: place the executable in ${CMAKE_BINARY_DIR}/bin instead of the package's binary directory.
# NO_TEST: build the executable only; the caller registers CTest entries itself.
function(sturdy_add_test package source)
  if(NOT STURDY_TESTS_ENABLED)
    return()
  endif()
  cmake_parse_arguments(TEST "KEEP_ASSERTS;BIN_DIR;NO_TEST" "NAME;TIMEOUT;C_STANDARD"
    "ARGS;DEFINES;LINK;DEPENDS" ${ARGN})

  cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE
    OUTPUT_VARIABLE _source)

  set(_name "${TEST_NAME}")
  if(NOT _name)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests"
      OUTPUT_VARIABLE _relative)
    cmake_path(REMOVE_EXTENSION _relative LAST_ONLY OUTPUT_VARIABLE _stem)
    string(REPLACE "/" "" _stem "${_stem}")
    if(_stem MATCHES "^${package}")
      set(_name "${_stem}")
    else()
      set(_name "${package}${_stem}")
    endif()
  endif()
  if(NOT TEST_TIMEOUT)
    set(TEST_TIMEOUT ${STURDY_DEFAULT_TEST_TIMEOUT})
  endif()

  add_executable("${_name}" "${_source}")
  # LINK first: static link order follows argument order.
  target_link_libraries("${_name}" PRIVATE ${TEST_LINK} "Sturdy::${package}")
  if(TEST_DEFINES)
    target_compile_definitions("${_name}" PRIVATE ${TEST_DEFINES})
  endif()
  if(TEST_DEPENDS)
    add_dependencies("${_name}" ${TEST_DEPENDS})
  endif()
  if(TEST_KEEP_ASSERTS)
    target_compile_options("${_name}" PRIVATE -UNDEBUG)
  endif()
  if(TEST_C_STANDARD)
    set_target_properties("${_name}" PROPERTIES
      C_STANDARD ${TEST_C_STANDARD} C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  endif()
  if(TEST_BIN_DIR)
    set_target_properties("${_name}" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  endif()
  sturdy_enable_warnings("${_name}")

  if(NOT TEST_NO_TEST)
    add_test(NAME "${_name}" COMMAND "${_name}" ${TEST_ARGS})
    set_tests_properties("${_name}" PROPERTIES TIMEOUT ${TEST_TIMEOUT})
  endif()
endfunction()

# Value for <key> in a list of "<key>=<value>" entries, or empty.
function(_sturdy_lookup out_var key)
  set(_value)
  foreach(_entry IN LISTS ARGN)
    if(_entry MATCHES "^([^=]+)=(.*)$")
      if("${CMAKE_MATCH_1}" STREQUAL "${key}")
        set(_value "${CMAKE_MATCH_2}")
      endif()
    endif()
  endforeach()
  set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(sturdy_add_tests package)
  if(NOT STURDY_TESTS_ENABLED)
    return()
  endif()
  cmake_parse_arguments(TESTS "" "TIMEOUT" "TIMEOUTS;NAMES;KEEP_ASSERTS;BIN_DIR;EXCLUDE" ${ARGN})

  sturdy_glob(_files RECURSE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*Test.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*Test.c")

  foreach(_file IN LISTS _files)
    cmake_path(RELATIVE_PATH _file BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests"
      OUTPUT_VARIABLE _relative)
    cmake_path(REMOVE_EXTENSION _relative LAST_ONLY OUTPUT_VARIABLE _key)
    if(_key IN_LIST TESTS_EXCLUDE)
      continue()
    endif()

    set(_options)
    _sturdy_lookup(_name "${_key}" ${TESTS_NAMES})
    if(_name)
      list(APPEND _options NAME "${_name}")
    endif()
    _sturdy_lookup(_timeout "${_key}" ${TESTS_TIMEOUTS})
    if(NOT _timeout)
      set(_timeout "${TESTS_TIMEOUT}")
    endif()
    if(_timeout)
      list(APPEND _options TIMEOUT "${_timeout}")
    endif()
    if(_key IN_LIST TESTS_KEEP_ASSERTS)
      list(APPEND _options KEEP_ASSERTS)
    endif()
    if(_key IN_LIST TESTS_BIN_DIR)
      list(APPEND _options BIN_DIR)
    endif()

    sturdy_add_test("${package}" "${_file}" ${_options})
  endforeach()
endfunction()

# A CTest entry that runs a cmake/tests/<script>.cmake script with -D<var>=<value> definitions.
#   sturdy_add_script_test(<name> <script> [TIMEOUT <s>] [DEFINES <var>=<value>...])
function(sturdy_add_script_test name script)
  if(NOT STURDY_TESTS_ENABLED)
    return()
  endif()
  cmake_parse_arguments(SCRIPT "" "TIMEOUT" "DEFINES" ${ARGN})
  if(NOT SCRIPT_TIMEOUT)
    set(SCRIPT_TIMEOUT ${STURDY_DEFAULT_TEST_TIMEOUT})
  endif()
  set(_definitions)
  foreach(_definition IN LISTS SCRIPT_DEFINES)
    list(APPEND _definitions "-D${_definition}")
  endforeach()
  add_test(NAME "${name}"
    COMMAND "${CMAKE_COMMAND}" ${_definitions} -P "${CMAKE_SOURCE_DIR}/cmake/tests/${script}.cmake")
  set_tests_properties("${name}" PROPERTIES TIMEOUT ${SCRIPT_TIMEOUT})
endfunction()

# Proves a static aggregate costs nothing when linked but unreferenced: a probe that links
# Sturdy::<package> and does nothing must start and exit silently. Only meaningful for static builds,
# where dead-stripping decides what survives. Names are <stem>Probe and <stem>Test.
#   sturdy_add_static_unused_test(<package> [NAME_STEM <stem>])   # default stem: <package>Unused
function(sturdy_add_static_unused_test package)
  if(NOT STURDY_TESTS_ENABLED OR STURDY_BUILD_SHARED_LIBS)
    return()
  endif()
  cmake_parse_arguments(UNUSED "" "NAME_STEM" "" ${ARGN})
  if(NOT UNUSED_NAME_STEM)
    set(UNUSED_NAME_STEM "${package}Unused")
  endif()
  set(_probe "${UNUSED_NAME_STEM}Probe")

  add_executable("${_probe}" "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp")
  target_link_libraries("${_probe}" PRIVATE "Sturdy::${package}")
  sturdy_enable_warnings("${_probe}")
  sturdy_add_script_test("${UNUSED_NAME_STEM}Test" AssertStaticAggregateUnused
    TIMEOUT 60
    DEFINES
      "STURDY_STATIC_PROBE=$<TARGET_FILE:${_probe}>"
      "STURDY_STATIC_AGGREGATE_LABEL=Sturdy::${package}")
endfunction()

# Fresh-configure regression test for an opt-out root option (see AssertFreshConfigureBoundary.cmake):
# configures a scratch tree with <option> and asserts <forbidden> appears nowhere in its build graph.
#   sturdy_add_fresh_configure_test(<name> LABEL <label> OPTION <VAR=VALUE> FORBIDDEN <string>)
function(sturdy_add_fresh_configure_test name)
  cmake_parse_arguments(FRESH "" "LABEL;OPTION;FORBIDDEN" "" ${ARGN})
  sturdy_add_script_test("${name}" AssertFreshConfigureBoundary
    TIMEOUT 300
    DEFINES
      "STURDY_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
      "STURDY_PROBE_LABEL=${FRESH_LABEL}"
      "STURDY_PROBE_OPTION=${FRESH_OPTION}"
      "STURDY_PROBE_FORBIDDEN_STRING=${FRESH_FORBIDDEN}"
      "STURDY_PROBE_ARCH=${STURDY_ARCH}"
      "STURDY_PROBE_OS=${STURDY_OS}"
      "STURDY_PROBE_COMPILER=${STURDY_COMPILER}"
      "STURDY_PROBE_C_COMPILER=${CMAKE_C_COMPILER}"
      "STURDY_PROBE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endfunction()
