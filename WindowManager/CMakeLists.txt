# Sturdy.WindowManager — window creation, input/event pumping, and native window effects.
# SDL3 is the built-in provider. Additional providers remain separate opt-in packages so normal
# WindowManager consumers do not inherit another window-system dependency.

# One per-OS source tree is compiled (FreeBSD shares Linux's); the rest are excluded.
sturdy_inactive_os_dirs(_sturdy_window_manager_excluded_source_dirs DIRS Linux MacOS Windows Web)
string(TOUPPER "${STURDY_OS}" _sturdy_window_manager_platform_upper)
set(_sturdy_window_manager_definitions
    STURDY_PLATFORM_${_sturdy_window_manager_platform_upper}=1
    STURDY_VIEW_OS_${_sturdy_window_manager_platform_upper}=1)

message(STATUS "WindowManager source view: ${STURDY_OS}")
message(STATUS "WindowManager built-in window provider: SDL3")

# Providers/GLFW is a separate optional package: sturdy_add_package() leaves out nested directories
# that have their own CMakeLists.txt, so its sources are not compiled into this target.
sturdy_add_package(WindowManager
    ENGINE_OPTIMIZED

    EXCLUDE_SOURCE_DIRS
        ${_sturdy_window_manager_excluded_source_dirs}

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::SDL3
        Sturdy::GLM
)

target_compile_definitions(WindowManager PUBLIC ${_sturdy_window_manager_definitions})

if(WIN32 AND STURDY_OS STREQUAL "Windows")
    target_link_libraries(WindowManager PUBLIC dwmapi)
endif()

if(STURDY_OS STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(STURDY_WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    pkg_check_modules(STURDY_WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.49)
    pkg_get_variable(STURDY_WAYLAND_PROTOCOLS_DATA_DIR wayland-protocols pkgdatadir)
    find_program(STURDY_WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

    set(_sturdy_wayland_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
    set(_sturdy_wayland_protocols_dir "${STURDY_WAYLAND_PROTOCOLS_DATA_DIR}/staging")
    set(_sturdy_wayland_protocol_sources)
    # <name> is the protocol's file stem: wayland-<name>-client-protocol.{h,c} come out of <xml>.
    foreach(_protocol IN ITEMS
        "ext-background-effect-v1|${_sturdy_wayland_protocols_dir}/ext-background-effect/ext-background-effect-v1.xml"
        "org-kde-kwin-blur|${CMAKE_CURRENT_SOURCE_DIR}/protocols/org-kde-kwin-blur.xml"
        "color-management-v1|${_sturdy_wayland_protocols_dir}/color-management/color-management-v1.xml")
        string(REPLACE "|" ";" _protocol "${_protocol}")
        list(GET _protocol 0 _name)
        list(GET _protocol 1 _xml)
        if(NOT EXISTS "${_xml}")
            message(FATAL_ERROR "Wayland protocol XML for ${_name} is missing at ${_xml} (wayland-protocols >= 1.49 required).")
        endif()

        set(_header "${_sturdy_wayland_generated_dir}/wayland-${_name}-client-protocol.h")
        set(_code "${_sturdy_wayland_generated_dir}/wayland-${_name}-client-protocol.c")
        add_custom_command(
            OUTPUT "${_header}" "${_code}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sturdy_wayland_generated_dir}"
            COMMAND "${STURDY_WAYLAND_SCANNER}" client-header "${_xml}" "${_header}"
            COMMAND "${STURDY_WAYLAND_SCANNER}" private-code "${_xml}" "${_code}"
            DEPENDS "${_xml}"
            VERBATIM
        )
        list(APPEND _sturdy_wayland_protocol_sources "${_header}" "${_code}")
    endforeach()

    target_sources(WindowManager PRIVATE ${_sturdy_wayland_protocol_sources})
    target_include_directories(WindowManager PRIVATE "${_sturdy_wayland_generated_dir}")
    target_link_libraries(WindowManager PRIVATE PkgConfig::STURDY_WAYLAND_CLIENT)
endif()

sturdy_add_tests(WindowManager TIMEOUTS WindowManagerHighRateInputTest=30)

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
