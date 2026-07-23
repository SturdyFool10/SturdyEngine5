cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR
        "SturdyEngine5 uses C++ module file sets and requires the Ninja generator for "
        "module dependency scanning plus compile_commands.json export. Configure with "
        "one of the presets in CMakePresets.json."
    )
endif()

project(SturdyEngine5
    VERSION 0.1.0
    DESCRIPTION "Sturdy Engine 5"
    LANGUAGES C CXX
)

include(CTest)

set(STURDY_CPP_STANDARD 23 CACHE STRING "C++ standard used by SturdyEngine targets. 23 is the latest published stable ISO C++ standard.")
set(CMAKE_CXX_STANDARD ${STURDY_CPP_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(STURDY_FETCH_DEPENDENCIES "Download third-party dependencies with FetchContent when packages are not already available." ON)
# Off by default: KhronosGroup/glTF-Sample-Assets is a large, ever-growing asset repo, not a build
# dependency — only needed when exercising the glTF import path (plans/gltf-import.md) against real
# content. Turn on to let Runtime's glTF demo hook find real sample models via FetchContent's usual
# shared-cache mechanism, same as any other STURDY_FETCH_DEPENDENCIES source.
option(STURDY_FETCH_SAMPLE_ASSETS "Fetch KhronosGroup/glTF-Sample-Assets for local glTF import testing." OFF)
option(STURDY_BUILD_SHARED_LIBS "Build SturdyEngine targets as shared libraries." OFF)
option(STURDY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for SturdyEngine targets." OFF)
option(STURDY_USE_COMPILER_LAUNCHER "Use sccache/ccache when available to speed repeated C/C++ builds." ON)
set(STURDY_COMPILER_LAUNCHER "" CACHE FILEPATH "Explicit compiler launcher executable. Overrides auto-detected sccache/ccache when set.")
option(STURDY_GLOB_CONFIGURE_DEPENDS "Re-check source globs during every build. Disable for faster incremental Ninja builds; re-run CMake after adding/removing files." OFF)
option(STURDY_INCLUDE_HEADERS_IN_TARGETS "Add headers to generated target source lists for IDE visibility. Disable for faster configure/generate." OFF)
option(STURDY_FETCH_UPDATES_DISCONNECTED "Skip FetchContent git update checks after dependencies are populated." ON)
# Force the Windows GUI (WinMain, no console) entry point in every configuration. When OFF, WinMain
# is used only for Windows Dist builds; Debug/RelWithDebInfo/Release keep the console main().
option(SFT_USE_WINMAIN "Always use the Windows WinMain entry point, regardless of build type." OFF)

if(NOT STURDY_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build third-party dependencies as shared libraries." FORCE)
endif()

if(STURDY_FETCH_UPDATES_DISCONNECTED)
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Skip FetchContent update checks" FORCE)
endif()

if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Dist)
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations." FORCE)
endif()

set(CMAKE_C_FLAGS_DIST "${CMAKE_C_FLAGS_RELEASE}" CACHE STRING "C flags for Dist builds." FORCE)
set(CMAKE_CXX_FLAGS_DIST "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "C++ flags for Dist builds." FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DIST "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "Executable linker flags for Dist builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DIST "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}" CACHE STRING "Shared library linker flags for Dist builds." FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_DIST "${CMAKE_STATIC_LINKER_FLAGS_RELEASE}" CACHE STRING "Static library linker flags for Dist builds." FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Arch/OS/Profile build matrix (validates STURDY_ARCH/STURDY_OS, applies cross --target flags)
# and build-throughput settings (ThinLTO for optimized profiles, lld, split DWARF). Both must
# run before dependencies and subdirectories so every target inherits them.
include(SturdyMatrix)
sturdy_apply_matrix()
include(SturdyBuildSpeed)

function(sturdy_configure_compiler_launcher)
    if(NOT STURDY_USE_COMPILER_LAUNCHER)
        return()
    endif()

    if(STURDY_COMPILER_LAUNCHER)
        set(_sturdy_compiler_launcher "${STURDY_COMPILER_LAUNCHER}")
    else()
        find_program(_sturdy_compiler_launcher NAMES sccache ccache)
    endif()

    if(_sturdy_compiler_launcher)
        set(CMAKE_C_COMPILER_LAUNCHER "${_sturdy_compiler_launcher}" CACHE FILEPATH "C compiler launcher" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_sturdy_compiler_launcher}" CACHE FILEPATH "C++ compiler launcher" FORCE)
        message(STATUS "Compiler launcher: ${_sturdy_compiler_launcher}")
    endif()
endfunction()

sturdy_configure_compiler_launcher()

include(SturdyDependencies)
include(SturdyLicenses)
include(SturdyPackage)

sturdy_configure_dependencies()
add_subdirectory(Foundation)
add_subdirectory(Async)
add_subdirectory(Ecs)
add_subdirectory(Text)
add_subdirectory(graphicsPlatform)
add_subdirectory(RHI)
add_subdirectory(Platform)
add_subdirectory(Core)
add_subdirectory(Renderer)
add_subdirectory(Engine)
add_subdirectory(Runtime)
