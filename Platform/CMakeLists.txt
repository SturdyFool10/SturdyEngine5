set(STURDY_VIEW_OS "" CACHE STRING "Override the platform source set for clangd View As profiles. Valid values: Windows, MacOS, Linux.")
set_property(CACHE STURDY_VIEW_OS PROPERTY STRINGS "" Windows MacOS Linux)

set(_sturdy_platform_definitions)
set(_sturdy_platform_excluded_source_dirs)

if(STURDY_VIEW_OS)
    if(NOT STURDY_VIEW_OS MATCHES "^(Windows|MacOS|Linux)$")
        message(FATAL_ERROR "Unsupported STURDY_VIEW_OS='${STURDY_VIEW_OS}'. Expected Windows, MacOS, Linux, or empty.")
    endif()
    set(_sturdy_effective_platform "${STURDY_VIEW_OS}")
else()
    if(WIN32)
        set(_sturdy_effective_platform Windows)
    elseif(APPLE)
        set(_sturdy_effective_platform MacOS)
    elseif(UNIX)
        set(_sturdy_effective_platform Linux)
    else()
        message(FATAL_ERROR "Unsupported platform. Add a Platform/<OS> implementation and CMake gate.")
    endif()
endif()

if(_sturdy_effective_platform STREQUAL "Windows")
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_WINDOWS=1)
    list(APPEND _sturdy_platform_excluded_source_dirs Linux MacOS)
elseif(_sturdy_effective_platform STREQUAL "MacOS")
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_MACOS=1)
    list(APPEND _sturdy_platform_excluded_source_dirs Linux Windows)
elseif(_sturdy_effective_platform STREQUAL "Linux")
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_LINUX=1)
    list(APPEND _sturdy_platform_excluded_source_dirs MacOS Windows)
else()
    message(FATAL_ERROR "Unsupported effective platform '${_sturdy_effective_platform}'.")
endif()

string(TOUPPER "${_sturdy_effective_platform}" _sturdy_effective_platform_upper)
list(APPEND _sturdy_platform_definitions STURDY_VIEW_OS_${_sturdy_effective_platform_upper}=1)

message(STATUS "Platform source view: ${_sturdy_effective_platform}")

sturdy_add_package(Platform
    EXCLUDE_SOURCE_DIRS
        ${_sturdy_platform_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::SDL3
        Sturdy::GLFW
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)

if(WIN32 AND _sturdy_effective_platform STREQUAL "Windows")
    target_link_libraries(Platform PUBLIC dwmapi)
endif()

target_compile_definitions(Platform PUBLIC ${_sturdy_platform_definitions})
