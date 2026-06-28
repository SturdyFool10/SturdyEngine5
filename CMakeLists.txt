cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR
        "SturdyEngine5 uses C++ module file sets and requires the Ninja generator for "
        "module dependency scanning plus compile_commands.json export. Configure with "
        "`cmake --preset ninja-relwithdebinfo`, `cmake --preset ninja-debug`, or "
        "`cmake --preset ninja-dist`."
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

# Mirror the active build's compile_commands.json to the source root so clangd
# (and any other tooling) discovers it by walking up from a source file, with no
# preset-specific path baked into .clangd. The link repoints on every configure,
# so the most recently configured preset is the one the editor sees. Symlink
# where possible, copy where symlinks are not permitted (e.g. some Windows
# setups); never fatal, since this is an editor convenience, not a build input.
if(CMAKE_EXPORT_COMPILE_COMMANDS AND NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    file(CREATE_LINK
        "${CMAKE_BINARY_DIR}/compile_commands.json"
        "${CMAKE_SOURCE_DIR}/compile_commands.json"
        RESULT _sturdy_compile_commands_link
        SYMBOLIC COPY_ON_ERROR
    )
    if(NOT _sturdy_compile_commands_link EQUAL 0)
        message(STATUS
            "Could not mirror compile_commands.json to the source root "
            "(${_sturdy_compile_commands_link}); point your editor at "
            "${CMAKE_BINARY_DIR}/compile_commands.json directly."
        )
    endif()
endif()
