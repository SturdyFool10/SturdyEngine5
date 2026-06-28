cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR
        "SturdyEngine5 uses C++ module file sets and requires the Ninja generator for "
        "module dependency scanning plus compile_commands.json export. Configure with "
        "one of the presets in CMakePresets.json."
    )
endif()

if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    find_program(STURDY_CLANG_C_COMPILER NAMES clang)
    if(STURDY_CLANG_C_COMPILER)
        set(CMAKE_C_COMPILER "${STURDY_CLANG_C_COMPILER}" CACHE FILEPATH "C compiler" FORCE)
    endif()
endif()

if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    find_program(STURDY_CLANG_CXX_COMPILER NAMES clang++)
    if(STURDY_CLANG_CXX_COMPILER)
        set(CMAKE_CXX_COMPILER "${STURDY_CLANG_CXX_COMPILER}" CACHE FILEPATH "C++ compiler" FORCE)
    endif()
endif()

project(SturdyEngine5
    VERSION 0.1.0
    DESCRIPTION "Sturdy Engine 5"
    LANGUAGES C CXX
)

set(STURDY_CPP_STANDARD 23 CACHE STRING "C++ standard used by SturdyEngine targets. 23 is the latest published stable ISO C++ standard.")
set(CMAKE_CXX_STANDARD ${STURDY_CPP_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(STURDY_FETCH_DEPENDENCIES "Download third-party dependencies with FetchContent when packages are not already available." ON)
option(STURDY_BUILD_SHARED_LIBS "Build SturdyEngine targets as shared libraries." OFF)
option(STURDY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for SturdyEngine targets." OFF)

if(NOT STURDY_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build third-party dependencies as shared libraries." FORCE)
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

include(SturdyDependencies)
include(SturdyPackage)

sturdy_configure_dependencies()
add_subdirectory(Foundation)
add_subdirectory(Platform)
add_subdirectory(Core)
add_subdirectory(Engine)
add_subdirectory(Runtime)
