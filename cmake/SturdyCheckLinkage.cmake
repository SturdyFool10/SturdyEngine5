# Post-link check for self-contained Linux executables (cmake/SturdySelfContained.cmake). Run with:
#   cmake -DBINARY=<exe> -DREADELF=<llvm-readelf|readelf> -DGLIBC_FLOOR=<x.y|empty> -DTOLERATED=<lib;lib> -P SturdyCheckLinkage.cmake
#
# Fails when the executable
#   * lists a NEEDED shared library that is not part of the C library (and not on the tolerated list), or
#   * references a glibc symbol version newer than GLIBC_FLOOR.
cmake_minimum_required(VERSION 3.20)

foreach(_required BINARY READELF)
    if(NOT ${_required})
        message(FATAL_ERROR "SturdyCheckLinkage: -D${_required}=... is required")
    endif()
endforeach()

execute_process(COMMAND "${READELF}" -d "${BINARY}" OUTPUT_VARIABLE _dynamic RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "SturdyCheckLinkage: ${READELF} -d failed on ${BINARY}: ${_err}")
endif()

# What every glibc (and musl) system provides.
set(_allowed_regex "^(libc\\.so\\.6|libm\\.so\\.6|libdl\\.so\\.2|libpthread\\.so\\.0|librt\\.so\\.1|libutil\\.so\\.1|ld-linux[A-Za-z0-9_.-]*\\.so\\.[0-9]+|libc\\.musl-[a-z0-9_]+\\.so\\.1)$")

string(REGEX MATCHALL "Shared library: \\[[^]]+\\]" _needed_lines "${_dynamic}")
set(_bad "")
set(_tolerated_found "")
foreach(_line IN LISTS _needed_lines)
    string(REGEX REPLACE "Shared library: \\[([^]]+)\\]" "\\1" _lib "${_line}")
    if(_lib MATCHES "${_allowed_regex}")
        continue()
    endif()
    list(FIND TOLERATED "${_lib}" _tolerated_index)
    if(_tolerated_index GREATER -1)
        list(APPEND _tolerated_found "${_lib}")
    else()
        list(APPEND _bad "${_lib}")
    endif()
endforeach()

# Newest glibc symbol version referenced.
execute_process(COMMAND "${READELF}" --dyn-syms --wide "${BINARY}" OUTPUT_VARIABLE _symbols)
string(REGEX MATCHALL "@GLIBC_[0-9]+(\\.[0-9]+)+" _versions "${_symbols}")
set(_newest "0")
set(_newest_symbol "")
foreach(_v IN LISTS _versions)
    string(SUBSTRING "${_v}" 7 -1 _number)
    if(_number VERSION_GREATER _newest)
        set(_newest "${_number}")
    endif()
endforeach()
if(NOT _newest STREQUAL "0")
    string(REGEX MATCH "[^ \t]+@GLIBC_${_newest}" _newest_symbol "${_symbols}")
endif()

get_filename_component(_name "${BINARY}" NAME)
set(_failed FALSE)
if(_bad)
    string(REPLACE ";" ", " _bad_text "${_bad}")
    message(SEND_ERROR "${_name} is not self-contained: it needs shared libraries the target system may not have: ${_bad_text}\n"
        "  Inspect the link line with `ninja -t commands ${_name}`; a /usr/lib/*.so input means a system package was picked up "
        "(STURDY_PREFER_SYSTEM_DEPENDENCIES must be OFF, or a dependency's find_package() found one).")
    set(_failed TRUE)
endif()
if(GLIBC_FLOOR AND _newest VERSION_GREATER GLIBC_FLOOR)
    message(SEND_ERROR "${_name} requires glibc ${_newest} (${_newest_symbol}), newer than the allowed floor ${GLIBC_FLOOR}.\n"
        "  Add the symbol to cmake/linux/glibc_compat.c, link against an older glibc (STURDY_LINUX_SYSROOT), or raise STURDY_GLIBC_FLOOR.")
    set(_failed TRUE)
endif()
if(_tolerated_found)
    string(REPLACE ";" ", " _tolerated_text "${_tolerated_found}")
    message(WARNING "${_name} still depends on ${_tolerated_text} (known gap, see cmake/SturdySelfContained.cmake).")
endif()
if(NOT _failed)
    message(STATUS "${_name}: self-contained (needs only the C library; newest glibc symbol ${_newest})")
else()
    message(FATAL_ERROR "${_name}: self-contained check failed")
endif()
