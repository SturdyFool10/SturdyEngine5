# Build-throughput configuration: LTO for optimized profiles, the lld linker, and split DWARF.
# Nothing here changes optimization levels — only how fast the toolchain gets there.
include_guard(GLOBAL)

option(STURDY_ENABLE_LTO "Enable link-time optimization (ThinLTO with clang) for Release/RelWithDebInfo/Dist." ON)
option(STURDY_USE_LLD "Link with lld when available (much faster than GNU ld, enables the ThinLTO cache)." ON)
option(STURDY_SPLIT_DWARF "Emit debug info to .dwo files on Linux (-gsplit-dwarf) so debug-info profiles link faster." ON)

# --------------------------------------------------------------------------------------------
# LTO. Debug intentionally stays off: LTO buys runtime speed at the cost of link time, which is
# the wrong trade for the edit-compile-run loop. Clang's IPO is ThinLTO, which parallelizes and
# (with lld below) caches per-module codegen, keeping optimized incremental links fast too.
if(STURDY_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _sturdy_ipo_supported OUTPUT _sturdy_ipo_output LANGUAGES C CXX)
    if(_sturdy_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DIST ON)
        message(STATUS "LTO: enabled for Release/RelWithDebInfo/Dist (ThinLTO with clang)")
    else()
        message(WARNING "LTO requested but not supported by this toolchain: ${_sturdy_ipo_output}")
    endif()
endif()

# --------------------------------------------------------------------------------------------
# lld. CMAKE_LINKER_TYPE (CMake >= 3.29) makes the driver pick lld for every target, including
# third-party FetchContent dependencies.
if(STURDY_USE_LLD AND NOT MSVC)
    find_program(STURDY_LLD_EXECUTABLE NAMES ld.lld lld)
    if(STURDY_LLD_EXECUTABLE)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
            set(CMAKE_LINKER_TYPE LLD)
        else()
            add_link_options(-fuse-ld=lld)
        endif()
        message(STATUS "Linker: lld (${STURDY_LLD_EXECUTABLE})")

        # ThinLTO cache: re-links after small changes reuse the previous run's per-module native
        # code instead of re-optimizing the world. ELF-lld flag, so Linux-host native builds only.
        if(STURDY_ENABLE_LTO AND _sturdy_ipo_supported AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            add_link_options(
                "$<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:Dist>>:LINKER:--thinlto-cache-dir=${CMAKE_BINARY_DIR}/.thinlto-cache>")
        endif()
    else()
        message(STATUS "Linker: system default (lld not found)")
    endif()
endif()

# --------------------------------------------------------------------------------------------
# Split DWARF. Debug info goes to .dwo files next to the objects instead of being copied into
# them, so static archives and final links move far fewer bytes on the profiles that carry
# debug info. Linux/ELF only; ccache and lld both understand it.
if(STURDY_SPLIT_DWARF AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT MSVC)
    add_compile_options(
        "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>>:-gsplit-dwarf>")
    message(STATUS "Debug info: split DWARF (-gsplit-dwarf) for Debug/RelWithDebInfo")
endif()
