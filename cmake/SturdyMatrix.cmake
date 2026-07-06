# The single source of truth for the SturdyEngine build matrix. Every consumer-facing
# configuration is a combination of the axes below; cmake/GeneratePresets.cmake expands them into
# CMakePresets.json (one preset per combination), and the root CMakeLists.txt includes this file
# to validate and apply the STURDY_ARCH / STURDY_OS / STURDY_COMPILER cache variables. Compiler is
# a lighter-weight axis than arch/os/profile: it does not get its own directory segment, only a
# "-gcc" preset-name/build-tree suffix when non-default (see sturdy_compiler_suffix()).
#
# This file must stay usable from both project mode (included by CMakeLists.txt) and script
# mode (included by `cmake -P` generators), so it only uses host-side facilities.
include_guard(GLOBAL)

set(STURDY_ARCH_LIST x86-64 Arm64 RISCV)
# Web is not a real architecture cross (wasm32 is fixed and owned entirely by the Emscripten
# toolchain file, not clang --target), so it only ever pairs with the canonical "x86-64" arch
# label — see the STURDY_OS STREQUAL "Web" guard in sturdy_apply_matrix() and the matching skip
# in cmake/GeneratePresets.cmake that keeps Arm64-Web/RISCV-Web combinations from being generated.
set(STURDY_OS_LIST Windows MacOS Linux FreeBSD Web)
set(STURDY_PROFILE_LIST Debug Release RelWithDebInfo Dist)
# Clang is the default/primary compiler (its universal --target flag is what makes the
# cross-architecture combinations above work at all). GCC is supported as an alternative for
# NATIVE builds only (STURDY_ARCH/STURDY_OS matching the host) — see the STURDY_COMPILER guard in
# sturdy_apply_matrix(). Real GCC cross-toolchains are separate per-target binaries (e.g.
# aarch64-linux-gnu-g++), not a single binary + flag like clang, and on MacOS the "gcc"/"g++" on
# PATH are actually clang in disguise (Apple ships no real GCC), so guessing at cross-GCC binary
# names is out of scope; a contributor who needs that can still point CMAKE_CXX_COMPILER/
# CMAKE_C_COMPILER at their own cross-GCC directly. Never applies to Web (Emscripten's toolchain
# is clang-based and not swappable to GCC), so it is skipped there in GeneratePresets.cmake.
set(STURDY_COMPILER_LIST Clang GCC)

# ---------------------------------------------------------------------------------------------
# Host detection (works in script mode, unlike CMAKE_SYSTEM_PROCESSOR).
function(sturdy_detect_host_arch out_var)
    cmake_host_system_information(RESULT _platform QUERY OS_PLATFORM)
    string(TOLOWER "${_platform}" _platform)
    if(_platform MATCHES "^(x86_64|amd64|x64)$")
        set(${out_var} "x86-64" PARENT_SCOPE)
    elseif(_platform MATCHES "^(arm64|aarch64)$")
        set(${out_var} "Arm64" PARENT_SCOPE)
    elseif(_platform MATCHES "^riscv")
        set(${out_var} "RISCV" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "Unrecognized host processor '${_platform}'. Add a mapping in cmake/SturdyMatrix.cmake.")
    endif()
endfunction()

function(sturdy_detect_host_os out_var)
    if(CMAKE_HOST_WIN32)
        set(${out_var} "Windows" PARENT_SCOPE)
    elseif(CMAKE_HOST_APPLE)
        set(${out_var} "MacOS" PARENT_SCOPE)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "FreeBSD")
        set(${out_var} "FreeBSD" PARENT_SCOPE)
    else()
        set(${out_var} "Linux" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------------------------
# Directory naming for build trees: lowercase, and Windows shortens to "win" because Slang's
# CMake source glob excludes any path containing /windows/ — a build directory named
# build/<arch>/windows/<profile> would silently drop Slang sources.
function(sturdy_os_dir_name os out_var)
    if(os STREQUAL "Windows")
        set(${out_var} "win" PARENT_SCOPE)
    else()
        string(TOLOWER "${os}" _lower)
        set(${out_var} "${_lower}" PARENT_SCOPE)
    endif()
endfunction()

# Clang target triple for a matrix combination, used when cross-compiling to a different
# architecture. OS cross-combinations configure as source views (for code intelligence); actually
# linking one requires the target platform's SDK/sysroot on top of the triple.
function(sturdy_target_triple arch os out_var)
    if(arch STREQUAL "x86-64")
        set(_cpu x86_64)
    elseif(arch STREQUAL "Arm64")
        set(_cpu aarch64)
    elseif(arch STREQUAL "RISCV")
        set(_cpu riscv64)
    else()
        message(FATAL_ERROR "Unknown STURDY_ARCH '${arch}'. Expected one of: ${STURDY_ARCH_LIST}")
    endif()

    if(os STREQUAL "Windows")
        set(_sys pc-windows-msvc)
    elseif(os STREQUAL "MacOS")
        set(_sys apple-darwin)
    elseif(os STREQUAL "Linux")
        set(_sys linux-gnu)
    elseif(os STREQUAL "FreeBSD")
        set(_sys unknown-freebsd)
    else()
        message(FATAL_ERROR "Unknown STURDY_OS '${os}'. Expected one of: ${STURDY_OS_LIST}")
    endif()

    set(${out_var} "${_cpu}-${_sys}" PARENT_SCOPE)
endfunction()

# Compiler suffix applied to preset names/build trees. Clang is unsuffixed (it's the default,
# and this keeps every existing preset name/build path exactly as it was before GCC support was
# added); GCC gets "-gcc" so the two compilers never collide on the same build tree — switching
# STURDY_COMPILER for the same arch/os/profile is a different preset with its own binaryDir,
# rather than CMake erroring on a build tree that changed compilers underneath it.
function(sturdy_compiler_suffix compiler out_var)
    if(compiler STREQUAL "GCC")
        set(${out_var} "-gcc" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------------------------
# Canonical naming derived from a matrix combination. These are the single source of truth for
# how a combination maps onto a CMake preset name and its build tree, so the preset generator
# (cmake/GeneratePresets.cmake) and the IDE bridge scripts (cmake/IDEBuild.cmake,
# cmake/ClangdView.cmake) never re-derive the convention independently and drift apart.
function(sturdy_preset_name arch os profile compiler out_var)
    string(TOLOWER "${arch}" _arch_dir)
    sturdy_os_dir_name("${os}" _os_dir)
    string(TOLOWER "${profile}" _profile_dir)
    sturdy_compiler_suffix("${compiler}" _compiler_suffix)
    set(${out_var} "${_arch_dir}-${_os_dir}-${_profile_dir}${_compiler_suffix}" PARENT_SCOPE)
endfunction()

# Build-tree directory (relative to the repository root) for a matrix combination.
function(sturdy_binary_dir arch os profile compiler out_var)
    string(TOLOWER "${arch}" _arch_dir)
    sturdy_os_dir_name("${os}" _os_dir)
    string(TOLOWER "${profile}" _profile_dir)
    sturdy_compiler_suffix("${compiler}" _compiler_suffix)
    set(${out_var} "build/${_arch_dir}/${_os_dir}/${_profile_dir}${_compiler_suffix}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------------------------
# Project-mode entry point: declares and validates the STURDY_ARCH / STURDY_OS cache variables,
# resolves the effective target OS for the platform source view, and applies --target flags for
# cross-architecture builds. Call once from the root CMakeLists.txt before any subdirectories.
macro(sturdy_apply_matrix)
    sturdy_detect_host_arch(STURDY_HOST_ARCH)
    sturdy_detect_host_os(STURDY_HOST_OS)

    set(STURDY_ARCH "${STURDY_HOST_ARCH}" CACHE STRING "Target CPU architecture. One of: ${STURDY_ARCH_LIST}")
    set_property(CACHE STURDY_ARCH PROPERTY STRINGS ${STURDY_ARCH_LIST})
    if(NOT STURDY_ARCH IN_LIST STURDY_ARCH_LIST)
        message(FATAL_ERROR "Unsupported STURDY_ARCH='${STURDY_ARCH}'. Expected one of: ${STURDY_ARCH_LIST}")
    endif()

    set(STURDY_OS "${STURDY_HOST_OS}" CACHE STRING "Target operating system (source view when not the host OS). One of: ${STURDY_OS_LIST}")
    set_property(CACHE STURDY_OS PROPERTY STRINGS ${STURDY_OS_LIST})

    # Legacy spelling used by older presets/tasks; wins when set so existing build trees keep
    # their meaning.
    set(STURDY_VIEW_OS "" CACHE STRING "Deprecated alias for STURDY_OS.")
    set_property(CACHE STURDY_VIEW_OS PROPERTY STRINGS "" ${STURDY_OS_LIST})
    if(STURDY_VIEW_OS)
        set(STURDY_OS "${STURDY_VIEW_OS}")
        message(STATUS "STURDY_VIEW_OS is deprecated; prefer STURDY_OS=${STURDY_VIEW_OS}")
    endif()

    if(NOT STURDY_OS IN_LIST STURDY_OS_LIST)
        message(FATAL_ERROR "Unsupported STURDY_OS='${STURDY_OS}'. Expected one of: ${STURDY_OS_LIST}")
    endif()

    set(STURDY_COMPILER "Clang" CACHE STRING "Compiler for native builds (ignored on Web, which always uses Emscripten's clang). One of: ${STURDY_COMPILER_LIST}")
    set_property(CACHE STURDY_COMPILER PROPERTY STRINGS ${STURDY_COMPILER_LIST})
    if(NOT STURDY_COMPILER IN_LIST STURDY_COMPILER_LIST)
        message(FATAL_ERROR "Unsupported STURDY_COMPILER='${STURDY_COMPILER}'. Expected one of: ${STURDY_COMPILER_LIST}")
    endif()

    # Profile: single-config generators get a validated default; multi-config generators carry
    # every profile at once (CMAKE_CONFIGURATION_TYPES) and ignore CMAKE_BUILD_TYPE.
    if(NOT CMAKE_CONFIGURATION_TYPES)
        if(NOT CMAKE_BUILD_TYPE)
            set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build profile. One of: ${STURDY_PROFILE_LIST}" FORCE)
            message(STATUS "No profile requested; defaulting to Debug")
        endif()
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${STURDY_PROFILE_LIST})
        if(NOT CMAKE_BUILD_TYPE IN_LIST STURDY_PROFILE_LIST)
            message(FATAL_ERROR "Unsupported profile CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'. Expected one of: ${STURDY_PROFILE_LIST}")
        endif()
    endif()

    if(STURDY_OS STREQUAL "Web")
        # wasm32 is not one of STURDY_ARCH_LIST's real CPUs and is never reached via clang
        # --target — the Emscripten toolchain file (loaded via the preset's "toolchainFile" before
        # project()) owns the compiler, sysroot, and target triple entirely. STURDY_ARCH is kept
        # only as the canonical "x86-64" label so this OS still fits the arch/os/profile naming
        # scheme; cmake/GeneratePresets.cmake never emits Arm64-Web/RISCV-Web combinations.
        if(NOT STURDY_ARCH STREQUAL "x86-64")
            message(FATAL_ERROR "STURDY_OS=Web only pairs with STURDY_ARCH=x86-64 (a naming convention, not a real cross-arch build).")
        endif()
        if(NOT CMAKE_TOOLCHAIN_FILE)
            message(FATAL_ERROR
                "STURDY_OS=Web requires the Emscripten CMake toolchain file to be set before "
                "project() — configure via the generated 'x86-64-web-<profile>' preset (which sets "
                "toolchainFile from $env{STURDY_EMSCRIPTEN_ROOT}), or pass "
                "-DCMAKE_TOOLCHAIN_FILE=<emscripten-root>/cmake/Modules/Platform/Emscripten.cmake yourself."
            )
        endif()
    else()
        if(STURDY_COMPILER STREQUAL "GCC" AND (NOT STURDY_ARCH STREQUAL STURDY_HOST_ARCH OR NOT STURDY_OS STREQUAL STURDY_HOST_OS))
            message(FATAL_ERROR
                "STURDY_COMPILER=GCC only supports native builds (STURDY_ARCH=${STURDY_HOST_ARCH}, "
                "STURDY_OS=${STURDY_HOST_OS} on this host) — unlike clang, GCC cross-compilation needs "
                "a distinct per-target compiler binary (e.g. aarch64-linux-gnu-g++), which this project "
                "does not guess at. Point CMAKE_CXX_COMPILER/CMAKE_C_COMPILER at your own cross-GCC "
                "directly if you have one, or use STURDY_COMPILER=Clang for cross builds."
            )
        endif()

        # Cross-architecture builds go through clang's --target. Same-arch builds stay flag-free so
        # native tooling (and any user-provided -march flags) behave exactly as before.
        if(NOT STURDY_ARCH STREQUAL STURDY_HOST_ARCH)
            sturdy_target_triple("${STURDY_ARCH}" "${STURDY_OS}" STURDY_TARGET_TRIPLE)
            add_compile_options(--target=${STURDY_TARGET_TRIPLE})
            add_link_options(--target=${STURDY_TARGET_TRIPLE})
            message(STATUS "Cross-architecture build: --target=${STURDY_TARGET_TRIPLE} (host is ${STURDY_HOST_ARCH})")
        endif()

        if(NOT STURDY_OS STREQUAL STURDY_HOST_OS)
            message(STATUS "STURDY_OS=${STURDY_OS} differs from the host (${STURDY_HOST_OS}): "
                "this configuration is a source view for code intelligence; linking needs the target SDK.")
        endif()
    endif()

    message(STATUS "Sturdy matrix: arch=${STURDY_ARCH} os=${STURDY_OS} profile=${CMAKE_BUILD_TYPE} compiler=${STURDY_COMPILER}")
endmacro()
