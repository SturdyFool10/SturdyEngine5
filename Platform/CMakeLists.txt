set(_sturdy_platform_sources)
set(_sturdy_platform_definitions)
file(GLOB_RECURSE _sturdy_window_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.cxx"
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.hh"
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Window/*.hxx"
)

if(WIN32)
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_WINDOWS=1)
    file(GLOB_RECURSE _sturdy_platform_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Windows/*.hxx"
    )
elseif(APPLE)
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_MACOS=1)
    file(GLOB_RECURSE _sturdy_platform_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MacOS/*.hxx"
    )
elseif(UNIX)
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_LINUX=1)
    file(GLOB_RECURSE _sturdy_platform_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.hh"
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Linux/*.hxx"
    )
else()
    message(FATAL_ERROR "Unsupported platform. Add a Platform/<OS> implementation and CMake gate.")
endif()

sturdy_add_package(Platform
    SOURCES
        ${_sturdy_window_sources}
        ${_sturdy_platform_sources}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::SDL3
        Sturdy::GLFW
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)

if(WIN32)
    target_link_libraries(Platform PUBLIC dwmapi)
endif()

target_compile_definitions(Platform PUBLIC ${_sturdy_platform_definitions})
