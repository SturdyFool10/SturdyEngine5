# Sturdy.WindowManager — window creation, input/event pumping, and native window effects.
# SDL3 is the built-in provider. Additional providers remain separate opt-in packages so normal
# WindowManager consumers do not inherit another window-system dependency.
set(_sturdy_window_manager_all_source_dirs Linux MacOS Windows Web)
set(_sturdy_window_manager_definitions)
set(_sturdy_window_manager_effective_platform "${STURDY_OS}")

if(_sturdy_window_manager_effective_platform STREQUAL "Windows")
    list(APPEND _sturdy_window_manager_definitions STURDY_PLATFORM_WINDOWS=1)
    set(_sturdy_window_manager_active_source_dir Windows)
elseif(_sturdy_window_manager_effective_platform STREQUAL "MacOS")
    list(APPEND _sturdy_window_manager_definitions STURDY_PLATFORM_MACOS=1)
    set(_sturdy_window_manager_active_source_dir MacOS)
elseif(_sturdy_window_manager_effective_platform STREQUAL "Linux")
    list(APPEND _sturdy_window_manager_definitions STURDY_PLATFORM_LINUX=1)
    set(_sturdy_window_manager_active_source_dir Linux)
elseif(_sturdy_window_manager_effective_platform STREQUAL "FreeBSD")
    list(APPEND _sturdy_window_manager_definitions STURDY_PLATFORM_FREEBSD=1)
    set(_sturdy_window_manager_active_source_dir Linux)
elseif(_sturdy_window_manager_effective_platform STREQUAL "Web")
    list(APPEND _sturdy_window_manager_definitions STURDY_PLATFORM_WEB=1)
    set(_sturdy_window_manager_active_source_dir Web)
else()
    message(FATAL_ERROR "Unsupported effective platform '${_sturdy_window_manager_effective_platform}'.")
endif()

set(_sturdy_window_manager_excluded_source_dirs ${_sturdy_window_manager_all_source_dirs})
list(REMOVE_ITEM _sturdy_window_manager_excluded_source_dirs "${_sturdy_window_manager_active_source_dir}")
string(TOUPPER "${_sturdy_window_manager_effective_platform}" _sturdy_window_manager_platform_upper)
list(APPEND _sturdy_window_manager_definitions STURDY_VIEW_OS_${_sturdy_window_manager_platform_upper}=1)

message(STATUS "WindowManager source view: ${_sturdy_window_manager_effective_platform}")
message(STATUS "WindowManager built-in window provider: SDL3")

sturdy_add_package(WindowManager
    EXCLUDE_SOURCE_DIRS
        tests
        # This subtree is a separate optional package/target. sturdy_add_package() recursively
        # globs the package directory, so without this exclusion its implementation sources are
        # accidentally compiled into the base WindowManager target as well.
        Providers/GLFW
        ${_sturdy_window_manager_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::SDL3
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)

target_compile_definitions(WindowManager PUBLIC ${_sturdy_window_manager_definitions})

if(WIN32 AND _sturdy_window_manager_effective_platform STREQUAL "Windows")
    target_link_libraries(WindowManager PUBLIC dwmapi)
endif()

if(_sturdy_window_manager_effective_platform STREQUAL "Linux")
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

    target_sources(WindowManager PRIVATE
        "${_sturdy_ext_background_effect_header}"
        "${_sturdy_ext_background_effect_code}"
        "${_sturdy_kde_blur_header}"
        "${_sturdy_kde_blur_code}"
        "${_sturdy_color_management_header}"
        "${_sturdy_color_management_code}"
    )
    target_include_directories(WindowManager PRIVATE "${_sturdy_wayland_generated_dir}")
    target_link_libraries(WindowManager PRIVATE PkgConfig::STURDY_WAYLAND_CLIENT)
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT _sturdy_window_manager_effective_platform STREQUAL "Web")
    add_executable(WindowManagerAccumulatorTest tests/WindowManagerAccumulatorTest.cpp)
    target_link_libraries(WindowManagerAccumulatorTest PRIVATE Sturdy::WindowManager)
    sturdy_enable_warnings(WindowManagerAccumulatorTest)
    add_test(NAME WindowManagerAccumulatorTest COMMAND WindowManagerAccumulatorTest)
    set_tests_properties(WindowManagerAccumulatorTest PROPERTIES TIMEOUT 10)

    add_executable(WindowManagerHighRateInputTest tests/WindowManagerHighRateInputTest.cpp)
    target_link_libraries(WindowManagerHighRateInputTest PRIVATE Sturdy::WindowManager)
    sturdy_enable_warnings(WindowManagerHighRateInputTest)
    add_test(NAME WindowManagerHighRateInputTest COMMAND WindowManagerHighRateInputTest)
    set_tests_properties(WindowManagerHighRateInputTest PROPERTIES TIMEOUT 30)
endif()

get_target_property(_sturdy_window_manager_sources WindowManager SOURCES)
foreach(_sturdy_window_manager_source IN LISTS _sturdy_window_manager_sources)
    file(TO_CMAKE_PATH "${_sturdy_window_manager_source}" _sturdy_window_manager_source_normalized)
    if(_sturdy_window_manager_source_normalized MATCHES "(^|/)Providers/GLFW(/|$)")
        message(FATAL_ERROR
            "Base WindowManager must not compile optional GLFW provider source: "
            "${_sturdy_window_manager_source}")
    endif()
endforeach()

get_target_property(_sturdy_window_manager_link_interface WindowManager INTERFACE_LINK_LIBRARIES)
foreach(_sturdy_window_manager_dependency IN LISTS _sturdy_window_manager_link_interface)
    if(_sturdy_window_manager_dependency MATCHES "[Gg][Ll][Ff][Ww]")
        message(FATAL_ERROR
            "Base WindowManager must not link optional GLFW provider dependencies: "
            "${_sturdy_window_manager_dependency}")
    endif()
endforeach()
