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

string(TOUPPER "${_sturdy_effective_platform}" _sturdy_effective_platform_upper)
list(APPEND _sturdy_platform_definitions STURDY_VIEW_OS_${_sturdy_effective_platform_upper}=1)

message(STATUS "Platform source view: ${_sturdy_effective_platform}")

# SDL3 is the built-in managed-Application default. Optional providers are separate packages so
# enabling their build does not add their symbols or dependencies to Sturdy::Platform consumers.
set(_sturdy_platform_windowing_deps Sturdy::SDL3)
message(STATUS "Platform built-in window provider: SDL3")

sturdy_add_package(Platform
    EXCLUDE_SOURCE_DIRS
        tests
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

if(_sturdy_effective_platform STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(STURDY_WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(STURDY_WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.49)
    pkg_get_variable(STURDY_WAYLAND_PROTOCOLS_DATA_DIR wayland-protocols pkgdatadir)
    find_program(STURDY_WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

    set(_sturdy_wayland_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
    set(_sturdy_ext_background_effect_xml
        "${STURDY_WAYLAND_PROTOCOLS_DATA_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml")
    set(_sturdy_kde_blur_xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/org-kde-kwin-blur.xml")
    set(_sturdy_color_management_xml
        "${STURDY_WAYLAND_PROTOCOLS_DATA_DIR}/staging/color-management/color-management-v1.xml")
    if(NOT EXISTS "${_sturdy_ext_background_effect_xml}")
        message(FATAL_ERROR
            "wayland-protocols >= 1.49 was found, but ext-background-effect-v1.xml is missing at "
            "${_sturdy_ext_background_effect_xml}.")
    endif()
    if(NOT EXISTS "${_sturdy_color_management_xml}")
        message(FATAL_ERROR
            "wayland-protocols >= 1.49 was found, but color-management-v1.xml is missing at "
            "${_sturdy_color_management_xml}.")
    endif()

    set(_sturdy_ext_background_effect_header
        "${_sturdy_wayland_generated_dir}/wayland-ext-background-effect-v1-client-protocol.h")
    set(_sturdy_ext_background_effect_code
        "${_sturdy_wayland_generated_dir}/wayland-ext-background-effect-v1-client-protocol.c")
    add_custom_command(
        OUTPUT "${_sturdy_ext_background_effect_header}" "${_sturdy_ext_background_effect_code}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sturdy_wayland_generated_dir}"
        COMMAND "${STURDY_WAYLAND_SCANNER}" client-header
            "${_sturdy_ext_background_effect_xml}" "${_sturdy_ext_background_effect_header}"
        COMMAND "${STURDY_WAYLAND_SCANNER}" private-code
            "${_sturdy_ext_background_effect_xml}" "${_sturdy_ext_background_effect_code}"
        DEPENDS "${_sturdy_ext_background_effect_xml}"
        VERBATIM
    )

    set(_sturdy_kde_blur_header
        "${_sturdy_wayland_generated_dir}/wayland-org-kde-kwin-blur-client-protocol.h")
    set(_sturdy_kde_blur_code
        "${_sturdy_wayland_generated_dir}/wayland-org-kde-kwin-blur-client-protocol.c")
    add_custom_command(
        OUTPUT "${_sturdy_kde_blur_header}" "${_sturdy_kde_blur_code}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sturdy_wayland_generated_dir}"
        COMMAND "${STURDY_WAYLAND_SCANNER}" client-header
            "${_sturdy_kde_blur_xml}" "${_sturdy_kde_blur_header}"
        COMMAND "${STURDY_WAYLAND_SCANNER}" private-code
            "${_sturdy_kde_blur_xml}" "${_sturdy_kde_blur_code}"
        DEPENDS "${_sturdy_kde_blur_xml}"
        VERBATIM
    )

    set(_sturdy_color_management_header
        "${_sturdy_wayland_generated_dir}/wayland-color-management-v1-client-protocol.h")
    set(_sturdy_color_management_code
        "${_sturdy_wayland_generated_dir}/wayland-color-management-v1-client-protocol.c")
    add_custom_command(
        OUTPUT "${_sturdy_color_management_header}" "${_sturdy_color_management_code}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sturdy_wayland_generated_dir}"
        COMMAND "${STURDY_WAYLAND_SCANNER}" client-header
            "${_sturdy_color_management_xml}" "${_sturdy_color_management_header}"
        COMMAND "${STURDY_WAYLAND_SCANNER}" private-code
            "${_sturdy_color_management_xml}" "${_sturdy_color_management_code}"
        DEPENDS "${_sturdy_color_management_xml}"
        VERBATIM
    )

    target_sources(Platform PRIVATE
        "${_sturdy_ext_background_effect_header}"
        "${_sturdy_ext_background_effect_code}"
        "${_sturdy_kde_blur_header}"
        "${_sturdy_kde_blur_code}"
        "${_sturdy_color_management_header}"
        "${_sturdy_color_management_code}"
    )
    target_include_directories(Platform PRIVATE "${_sturdy_wayland_generated_dir}")
    target_link_libraries(Platform PRIVATE PkgConfig::STURDY_WAYLAND_CLIENT)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT _sturdy_effective_platform STREQUAL "Web")
    add_executable(PlatformWindowManagerAccumulatorTest tests/WindowManagerAccumulatorTest.cpp)
    target_link_libraries(PlatformWindowManagerAccumulatorTest PRIVATE Sturdy::Platform)
    sturdy_enable_warnings(PlatformWindowManagerAccumulatorTest)
    add_test(NAME PlatformWindowManagerAccumulatorTest COMMAND PlatformWindowManagerAccumulatorTest)
    set_tests_properties(PlatformWindowManagerAccumulatorTest PROPERTIES TIMEOUT 10)

    # Sustained 8kHz synthetic mouse stream against the real DedicatedEventThread poll_loop() path;
    # runs for ~0.75s of real wall time by design (see the test file's own doc comment), so it gets a
    # much longer TIMEOUT than the deterministic accumulator test above.
    add_executable(PlatformWindowManagerHighRateInputTest tests/WindowManagerHighRateInputTest.cpp)
    target_link_libraries(PlatformWindowManagerHighRateInputTest PRIVATE Sturdy::Platform)
    sturdy_enable_warnings(PlatformWindowManagerHighRateInputTest)
    add_test(NAME PlatformWindowManagerHighRateInputTest COMMAND PlatformWindowManagerHighRateInputTest)
    set_tests_properties(PlatformWindowManagerHighRateInputTest PROPERTIES TIMEOUT 30)
endif()

get_target_property(
    _sturdy_platform_link_interface
    Platform
    INTERFACE_LINK_LIBRARIES
)
foreach(_sturdy_platform_dependency IN LISTS _sturdy_platform_link_interface)
    if(_sturdy_platform_dependency MATCHES "[Gg][Ll][Ff][Ww]")
        message(FATAL_ERROR
            "Base Platform must not link optional GLFW provider dependencies: "
            "${_sturdy_platform_dependency}"
        )
    endif()
endforeach()
