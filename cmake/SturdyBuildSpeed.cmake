# Build-throughput configuration: LTO for optimized profiles, the lld linker, and split DWARF.
# Nothing here changes optimization levels — only how fast the toolchain gets there.
include_guard(GLOBAL)

# All three techniques below are ELF/native-linker concerns (CMAKE_LINKER_TYPE=LLD, ThinLTO's
# check_ipo_supported() try-compile, and -gsplit-dwarf) that the Emscripten toolchain either
# rejects outright (LINKER_TYPE LLD is not a recognized value there — wasm-ld is already wired in
# by the toolchain file) or has no use for. check_ipo_supported()'s try-compile also runs as an
# isolated nested configure that reads ambient LDFLAGS/CXXFLAGS directly instead of inheriting
# this project's cache overrides, so on hosts that export distro-wide flags (e.g. Arch/CachyOS's
# -march=native / --sort-common linker defaults) it fails long before reaching the real question
# of IPO support. Skip all of it for Web.
if(STURDY_OS STREQUAL "Web")
    message(STATUS "Build speed: skipped (Emscripten toolchain owns LTO/linker/debug-info flags on Web)")
    return()
endif()

option(STURDY_ENABLE_LTO "Enable link-time optimization (ThinLTO with clang) for Release/RelWithDebInfo/Dist." ON)
option(STURDY_USE_LLD "Link with lld when available (much faster than GNU ld, enables the ThinLTO cache)." ON)
option(STURDY_SPLIT_DWARF "Emit debug info to .dwo files on Linux (-gsplit-dwarf) so debug-info profiles link faster." ON)

# --------------------------------------------------------------------------------------------
# LTO. Debug intentionally stays off: LTO buys runtime speed at the cost of link time, which is
# the wrong trade for the edit-compile-run loop. Clang's IPO is ThinLTO, which parallelizes and
# (with lld below) caches per-module codegen, keeping optimized incremental links fast too. GCC's
# IPO is its own WHOPR/fat-LTO scheme (not ThinLTO — that name and bitcode format are LLVM-only),
# enabled through the same CMAKE_INTERPROCEDURAL_OPTIMIZATION property either way.
if(STURDY_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _sturdy_ipo_supported OUTPUT _sturdy_ipo_output LANGUAGES C CXX)
    if(_sturdy_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DIST ON)
        if(STURDY_COMPILER STREQUAL "GCC")
            message(STATUS "LTO: enabled for Release/RelWithDebInfo/Dist (GCC's own WHOPR LTO)")
        else()
            message(STATUS "LTO: enabled for Release/RelWithDebInfo/Dist (ThinLTO with clang)")
        endif()
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
        # --thinlto-cache-dir is an LLVM-bitcode-only lld feature — GCC's LTO objects are not
        # ThinLTO bitcode, so this stays clang-only even though both compilers can use lld itself.
        if(STURDY_ENABLE_LTO AND _sturdy_ipo_supported AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT STURDY_COMPILER STREQUAL "GCC")
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
