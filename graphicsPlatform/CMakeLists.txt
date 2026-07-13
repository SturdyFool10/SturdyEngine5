# graphicsPlatform — classic C++ support code for graphics-adjacent OS/display queries.
#
# This package intentionally does not use C++ modules. It provides platform services that RHI/Core
# backends can use without depending on the higher-level Platform windowing package: HDR display
# metadata, display/output identity, and future graphics-present integration helpers.
set(_sturdy_graphics_platform_definitions)
set(_sturdy_graphics_platform_excluded_source_dirs)
set(_sturdy_graphics_platform_effective_os "${STURDY_OS}")
set(_sturdy_graphics_platform_effective_arch "${STURDY_ARCH}")

if(_sturdy_graphics_platform_effective_os STREQUAL "Windows")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_WINDOWS=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs Source/Linux Source/MacOS Source/FreeBSD Source/Web)
elseif(_sturdy_graphics_platform_effective_os STREQUAL "MacOS")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_MACOS=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs Source/Linux Source/Windows Source/FreeBSD Source/Web)
elseif(_sturdy_graphics_platform_effective_os STREQUAL "Linux")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_LINUX=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs Source/MacOS Source/Windows Source/FreeBSD Source/Web)
elseif(_sturdy_graphics_platform_effective_os STREQUAL "FreeBSD")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_FREEBSD=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs Source/Linux Source/MacOS Source/Windows Source/Web)
elseif(_sturdy_graphics_platform_effective_os STREQUAL "Web")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_WEB=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs Source/Linux Source/MacOS Source/Windows Source/FreeBSD)
else()
    message(FATAL_ERROR "Unsupported graphicsPlatform OS '${_sturdy_graphics_platform_effective_os}'.")
endif()

# The source tree is organized OS -> Arch. Keep non-matching arch directories out of the build so
# platform files can use arch-specific OS APIs/intrinsics without preprocessor soup.
if(_sturdy_graphics_platform_effective_arch STREQUAL "x86-64")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_ARCH_X86_64=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs
        Source/Linux/Arm64 Source/Linux/RISCV
        Source/Windows/Arm64 Source/Windows/RISCV
        Source/MacOS/Arm64 Source/MacOS/RISCV
        Source/FreeBSD/Arm64 Source/FreeBSD/RISCV
        Source/Web/Arm64 Source/Web/RISCV)
elseif(_sturdy_graphics_platform_effective_arch STREQUAL "Arm64")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_ARCH_ARM64=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs
        Source/Linux/x86-64 Source/Linux/RISCV
        Source/Windows/x86-64 Source/Windows/RISCV
        Source/MacOS/x86-64 Source/MacOS/RISCV
        Source/FreeBSD/x86-64 Source/FreeBSD/RISCV
        Source/Web/x86-64 Source/Web/RISCV)
elseif(_sturdy_graphics_platform_effective_arch STREQUAL "RISCV")
    list(APPEND _sturdy_graphics_platform_definitions STURDY_GRAPHICS_PLATFORM_ARCH_RISCV64=1)
    list(APPEND _sturdy_graphics_platform_excluded_source_dirs
        Source/Linux/x86-64 Source/Linux/Arm64
        Source/Windows/x86-64 Source/Windows/Arm64
        Source/MacOS/x86-64 Source/MacOS/Arm64
        Source/FreeBSD/x86-64 Source/FreeBSD/Arm64
        Source/Web/x86-64 Source/Web/Arm64)
else()
    message(FATAL_ERROR "Unsupported graphicsPlatform arch '${_sturdy_graphics_platform_effective_arch}'.")
endif()

sturdy_add_package(graphicsPlatform
    EXCLUDE_SOURCE_DIRS
        ${_sturdy_graphics_platform_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation

    PUBLIC_DEFINES
        ${_sturdy_graphics_platform_definitions}
)

if(WIN32 AND _sturdy_graphics_platform_effective_os STREQUAL "Windows")
    target_link_libraries(graphicsPlatform PUBLIC dxgi)
endif()
