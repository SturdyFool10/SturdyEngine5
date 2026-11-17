# PipeWire's client headers (and only the headers) for Audio/PipeWireOutput.cpp.
#
# The native PipeWire output opens libpipewire-0.3 with dlopen at run time, so nothing here is linked. It still has to be compiled against
# the client API's structs and the header-only SPA pod builder; fetching them pins that API and keeps the build independent of whether
# the build machine has PipeWire's development package. PipeWire's client ABI is stable, so a binary built against these headers works with
# any PipeWire from 0.3 on (the code uses only functions that old). Checked out once into the shared dependency cache.
include_guard(GLOBAL)

set(STURDY_PIPEWIRE_HEADERS_TAG "1.0.9" CACHE STRING "PipeWire release whose client headers PipeWireOutput.cpp is compiled against.")

function(sturdy_fetch_pipewire_headers)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR TARGET Sturdy::PipeWireHeaders)
        return()
    endif()
    set(_root "${STURDY_DEPS_CACHE_DIR}")
    set(_src "${_root}/pipewire-${STURDY_PIPEWIRE_HEADERS_TAG}-src")
    if(NOT EXISTS "${_src}/spa/include/spa/param/audio/format-utils.h")
        file(REMOVE_RECURSE "${_src}")
        message(STATUS "PipeWire headers: fetching ${STURDY_PIPEWIRE_HEADERS_TAG}")
        execute_process(
            COMMAND git clone --quiet --depth 1 --branch "${STURDY_PIPEWIRE_HEADERS_TAG}" https://gitlab.freedesktop.org/pipewire/pipewire.git "${_src}"
            RESULT_VARIABLE _rc ERROR_VARIABLE _err)
        if(NOT _rc EQUAL 0)
            # Not fatal: without the headers PipeWireOutput.cpp compiles to stubs and the sink uses PulseAudio/ALSA/JACK only.
            message(WARNING "PipeWire headers could not be fetched (${_err}); the native PipeWire audio output is left out of this build.")
            file(REMOVE_RECURSE "${_src}")
            return()
        endif()
    endif()

    # The one generated header the client API includes.
    string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _ "${STURDY_PIPEWIRE_HEADERS_TAG}")
    set(PIPEWIRE_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PIPEWIRE_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(PIPEWIRE_VERSION_MICRO "${CMAKE_MATCH_3}")
    set(PIPEWIRE_API_VERSION "\"0.3\"")
    set(_generated "${CMAKE_BINARY_DIR}/generated/pipewire-headers")
    configure_file("${_src}/src/pipewire/version.h.in" "${_generated}/pipewire/version.h" @ONLY)

    add_library(Sturdy::PipeWireHeaders INTERFACE IMPORTED GLOBAL)
    set_target_properties(Sturdy::PipeWireHeaders PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_src}/src;${_src}/spa/include;${_generated}")
    sturdy_register_license(pipewire "${_src}")
endfunction()
