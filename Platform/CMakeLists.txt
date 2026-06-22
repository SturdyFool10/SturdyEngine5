set(_sturdy_platform_definitions)
set(_sturdy_platform_excluded_source_dirs)

if(WIN32)
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_WINDOWS=1)
    list(APPEND _sturdy_platform_excluded_source_dirs Linux MacOS)
elseif(APPLE)
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_MACOS=1)
    list(APPEND _sturdy_platform_excluded_source_dirs Linux Windows)
elseif(UNIX)
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_LINUX=1)
    list(APPEND _sturdy_platform_excluded_source_dirs MacOS Windows)
else()
    message(FATAL_ERROR "Unsupported platform. Add a Platform/<OS> implementation and CMake gate.")
endif()

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

if(WIN32)
    target_link_libraries(Platform PUBLIC dwmapi)
endif()

target_compile_definitions(Platform PUBLIC ${_sturdy_platform_definitions})
