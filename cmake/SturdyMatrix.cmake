# The single source of truth for the SturdyEngine build matrix. Every consumer-facing
# configuration is a combination of the three axes below; cmake/GeneratePresets.cmake expands
# them into CMakePresets.json (one preset per combination), and the root CMakeLists.txt includes
# this file to validate and apply the STURDY_ARCH / STURDY_OS cache variables.
#
# This file must stay usable from both project mode (included by CMakeLists.txt) and script
# mode (included by `cmake -P` generators), so it only uses host-side facilities.
include_guard(GLOBAL)

set(STURDY_ARCH_LIST x86-64 Arm64 RISCV)
set(STURDY_OS_LIST Windows MacOS Linux FreeBSD)
set(STURDY_PROFILE_LIST Debug Release RelWithDebInfo Dist)

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

# ---------------------------------------------------------------------------------------------
# Canonical naming derived from a matrix combination. These are the single source of truth for
# how a combination maps onto a CMake preset name and its build tree, so the preset generator
# (cmake/GeneratePresets.cmake) and the IDE bridge scripts (cmake/IDEBuild.cmake,
# cmake/ClangdView.cmake) never re-derive the convention independently and drift apart.
function(sturdy_preset_name arch os profile out_var)
    string(TOLOWER "${arch}" _arch_dir)
    sturdy_os_dir_name("${os}" _os_dir)
    string(TOLOWER "${profile}" _profile_dir)
    set(${out_var} "${_arch_dir}-${_os_dir}-${_profile_dir}" PARENT_SCOPE)
endfunction()

# Build-tree directory (relative to the repository root) for a matrix combination.
function(sturdy_binary_dir arch os profile out_var)
    string(TOLOWER "${arch}" _arch_dir)
    sturdy_os_dir_name("${os}" _os_dir)
    string(TOLOWER "${profile}" _profile_dir)
    set(${out_var} "build/${_arch_dir}/${_os_dir}/${_profile_dir}" PARENT_SCOPE)
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

    message(STATUS "Sturdy matrix: arch=${STURDY_ARCH} os=${STURDY_OS} profile=${CMAKE_BUILD_TYPE}")
endmacro()
