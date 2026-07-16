# The platform source view follows the build matrix's target OS (STURDY_OS, resolved and
# validated by sturdy_apply_matrix() in the root CMakeLists.txt; the legacy STURDY_VIEW_OS
# spelling is honored there as well).
#
# Every per-OS source tree under src/Platform/ is listed here once; the active platform's tree is
# removed from the exclusion set below rather than each branch separately listing "every dir but
# mine" — that enumeration previously drifted (Web was missing from every non-Web branch's
# exclusion list, silently pulling Platform/Web/WindowEffectsImpl.cpp's fallback stubs into every
# desktop build) and a new platform dir would be one more place to forget to update.
set(_sturdy_platform_all_source_dirs Linux MacOS Windows Web)
set(_sturdy_platform_definitions)
set(_sturdy_effective_platform "${STURDY_OS}")

if(_sturdy_effective_platform STREQUAL "Windows")
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_WINDOWS=1)
    set(_sturdy_platform_active_source_dir Windows)
elseif(_sturdy_effective_platform STREQUAL "MacOS")
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_MACOS=1)
    set(_sturdy_platform_active_source_dir MacOS)
elseif(_sturdy_effective_platform STREQUAL "Linux")
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_LINUX=1)
    set(_sturdy_platform_active_source_dir Linux)
elseif(_sturdy_effective_platform STREQUAL "FreeBSD")
    # FreeBSD has no dedicated source tree yet: it reuses the Linux implementation (both are POSIX
    # + X11/Wayland via the same SDL3/GLFW paths). The Linux sources gate their OS-specific bodies
    # on __linux__, so on FreeBSD they compile to their portable fallbacks — a valid starting point
    # that links today. Flesh out __FreeBSD__ branches (or a Platform/FreeBSD tree) when needed.
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_FREEBSD=1)
    set(_sturdy_platform_active_source_dir Linux)
elseif(_sturdy_effective_platform STREQUAL "Web")
    # Browsers have no native window handle / OS chrome effects (see Platform/Web/
    # WindowEffectsImpl.cpp), so none of the desktop trees apply. GLFW itself is also excluded
    # (see the windowing deps comment below) since its sources are never linkable on Web.
    list(APPEND _sturdy_platform_definitions STURDY_PLATFORM_WEB=1)
    set(_sturdy_platform_active_source_dir Web)
else()
    message(FATAL_ERROR "Unsupported effective platform '${_sturdy_effective_platform}'.")
endif()

set(_sturdy_platform_excluded_source_dirs ${_sturdy_platform_all_source_dirs})
list(REMOVE_ITEM _sturdy_platform_excluded_source_dirs "${_sturdy_platform_active_source_dir}")
if(_sturdy_effective_platform STREQUAL "Web")
    list(APPEND _sturdy_platform_excluded_source_dirs Window/GLFW)
endif()

string(TOUPPER "${_sturdy_effective_platform}" _sturdy_effective_platform_upper)
list(APPEND _sturdy_platform_definitions STURDY_VIEW_OS_${_sturdy_effective_platform_upper}=1)

message(STATUS "Platform source view: ${_sturdy_effective_platform}")

# GLFW has no official Emscripten/wasm support (unlike SDL3, which is actively maintained there),
# so Web builds only ever get the SDL3 windowing backend — see Window::create<Backend> call sites,
# which must use SDL3::SDL3Window on Web since GLFW::GLFWWindow will not be compiled in.
set(_sturdy_platform_windowing_deps Sturdy::SDL3)
if(NOT _sturdy_effective_platform STREQUAL "Web")
    list(APPEND _sturdy_platform_windowing_deps Sturdy::GLFW)
endif()

sturdy_add_package(Platform
    EXCLUDE_SOURCE_DIRS
        ${_sturdy_platform_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        ${_sturdy_platform_windowing_deps}
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)

if(WIN32 AND _sturdy_effective_platform STREQUAL "Windows")
    target_link_libraries(Platform PUBLIC dwmapi)
endif()

target_compile_definitions(Platform PUBLIC ${_sturdy_platform_definitions})
