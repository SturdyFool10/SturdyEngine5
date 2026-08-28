# Optional GLFW window provider. This archive is deliberately separate from Sturdy::WindowManager:
# configuring it makes the provider available, while a product pays its link/runtime footprint only
# by explicitly referencing GLFW::create_window and linking Sturdy::WindowManagerGLFW.
set(_sturdy_glfw_provider_platform_dirs Linux MacOS Windows)
if(STURDY_OS STREQUAL "Windows")
    set(_sturdy_glfw_provider_active_dir Windows)
elseif(STURDY_OS STREQUAL "MacOS")
    set(_sturdy_glfw_provider_active_dir MacOS)
else()
    # Linux and FreeBSD share the POSIX provider source view, matching WindowManager's source selection.
    set(_sturdy_glfw_provider_active_dir Linux)
endif()
set(_sturdy_glfw_provider_excluded_dirs ${_sturdy_glfw_provider_platform_dirs})
list(REMOVE_ITEM _sturdy_glfw_provider_excluded_dirs "${_sturdy_glfw_provider_active_dir}")

sturdy_add_package(WindowManagerGLFW
    EXCLUDE_SOURCE_DIRS
        tests
        ${_sturdy_glfw_provider_excluded_dirs}

    PUBLIC_DEPS
        Sturdy::WindowManager
        # The concrete GLFWWindow API exposes GLFW types. This dependency is scoped to the optional
        # provider target; base WindowManager/Engine/Runtime consumers never inherit it.
        Sturdy::GLFW

    PRIVATE_DEPS
        # glfw3.h unconditionally pulls in <vulkan/vulkan.h> unless GLFW_INCLUDE_NONE is defined
        # first, and GLFWImpl.cpp deliberately doesn't define it (this engine creates Vulkan
        # surfaces through GLFW) — private since only the .cpp needs Vulkan headers, not this
        # package's own public surface.
        Sturdy::Vulkan

    DEBUG_PUBLIC_DEPS
)

if(STURDY_OS STREQUAL "Windows")
    # Native IME composition support (WindowManager/Providers/GLFW/Windows/GlfwWindowNative.cpp): imm32 for the IMM32
    # API itself, comctl32 for SetWindowSubclass/RemoveWindowSubclass/DefSubclassProc. Neither is an
    # MSVC default lib, so both need an explicit link — same bare-name pattern Sturdy::DirectX
    # already uses for d3d12/dxgi/dxguid (cmake/SturdyDependencies.cmake).
    target_link_libraries(WindowManagerGLFW PRIVATE imm32 comctl32)
elseif(_sturdy_glfw_provider_active_dir STREQUAL "Linux")
    # X11 IME composition support (WindowManager/Providers/GLFW/Linux/GlfwWindowNative.cpp, XIMPreeditCallbacks path):
    # libX11 for Xlib itself. GLFW's own X11 backend already requires libX11 to exist on the system
    # to build at all, so this is never introducing a new hard dependency, only linking a library
    # that's already present.
    find_package(X11 REQUIRED)
    target_link_libraries(WindowManagerGLFW PRIVATE X11::X11)

    # Wayland IME composition support (same file, zwp_text_input_v3 path): this repo's own
    # wayland-scanner codegen step for a protocol GLFW itself doesn't use — deliberately independent
    # of GLFW's own protocol codegen (cache/deps/glfw-src/src/CMakeLists.txt's generate_wayland_
    # protocol() macro), which only runs inside GLFW's own build and produces headers private to it.
    # Mirrors that macro's two-command shape (client-header, private-code) against our own vendored
    # copy of the protocol XML (deps/wayland/text-input-unstable-v3.xml, fetched from the upstream
    # wayland-protocols repository) rather than GLFW's.
    find_program(STURDY_WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
    find_package(PkgConfig)
    if(STURDY_WAYLAND_SCANNER_EXECUTABLE AND PkgConfig_FOUND)
        pkg_check_modules(STURDY_WAYLAND_CLIENT QUIET wayland-client)
    endif()
    if(STURDY_WAYLAND_SCANNER_EXECUTABLE AND STURDY_WAYLAND_CLIENT_FOUND)
        set(_sturdy_wayland_protocol_xml "${CMAKE_CURRENT_SOURCE_DIR}/deps/wayland/text-input-unstable-v3.xml")
        set(_sturdy_wayland_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/wayland")
        set(_sturdy_wayland_header "${_sturdy_wayland_generated_dir}/text-input-unstable-v3-client-protocol.h")
        set(_sturdy_wayland_code "${_sturdy_wayland_generated_dir}/text-input-unstable-v3-client-protocol-code.h")
        file(MAKE_DIRECTORY "${_sturdy_wayland_generated_dir}")

        add_custom_command(
            OUTPUT "${_sturdy_wayland_header}"
            COMMAND "${STURDY_WAYLAND_SCANNER_EXECUTABLE}" client-header
                    "${_sturdy_wayland_protocol_xml}" "${_sturdy_wayland_header}"
            DEPENDS "${_sturdy_wayland_protocol_xml}"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${_sturdy_wayland_code}"
            COMMAND "${STURDY_WAYLAND_SCANNER_EXECUTABLE}" private-code
                    "${_sturdy_wayland_protocol_xml}" "${_sturdy_wayland_code}"
            DEPENDS "${_sturdy_wayland_protocol_xml}"
            VERBATIM
        )
        add_custom_target(WindowManagerGLFWWaylandTextInputProtocol
            DEPENDS "${_sturdy_wayland_header}" "${_sturdy_wayland_code}"
        )
        add_dependencies(WindowManagerGLFW WindowManagerGLFWWaylandTextInputProtocol)

        # The generated "-code.h" is #included directly by GlfwWindowNative.cpp (exactly how GLFW's
        # own wl_init.c consumes its equivalently-generated files) rather than compiled as its own
        # translation unit, so only the include path is needed here, not a source/library target.
        target_include_directories(WindowManagerGLFW PRIVATE "${_sturdy_wayland_generated_dir}")
        target_compile_definitions(WindowManagerGLFW PRIVATE STURDY_GLFW_WAYLAND_TEXT_INPUT_V3=1)
        target_link_libraries(WindowManagerGLFW PRIVATE ${STURDY_WAYLAND_CLIENT_LIBRARIES})
        target_include_directories(WindowManagerGLFW PRIVATE ${STURDY_WAYLAND_CLIENT_INCLUDE_DIRS})
    else()
        message(WARNING
            "wayland-scanner and/or the wayland-client pkg-config module were not found; the GLFW "
            "provider's Wayland IME composition support will be compiled out (X11 IME and "
            "committed-text input on both are unaffected).")
    endif()
endif()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(WindowManagerGLFWFactoryAccessTest tests/FactoryAccessTest.cpp)
    target_link_libraries(WindowManagerGLFWFactoryAccessTest PRIVATE Sturdy::WindowManagerGLFW)
    sturdy_enable_warnings(WindowManagerGLFWFactoryAccessTest)
    add_test(NAME WindowManagerGLFWFactoryAccessTest COMMAND WindowManagerGLFWFactoryAccessTest)
    set_tests_properties(WindowManagerGLFWFactoryAccessTest PROPERTIES TIMEOUT 10)

    add_executable(GlfwWindowEventAndManagerTest tests/WindowEventAndManagerTest.cpp)
    target_link_libraries(GlfwWindowEventAndManagerTest PRIVATE Sturdy::WindowManagerGLFW)
    sturdy_enable_warnings(GlfwWindowEventAndManagerTest)
    add_test(NAME GlfwWindowEventAndManagerTest COMMAND GlfwWindowEventAndManagerTest)
    set_tests_properties(GlfwWindowEventAndManagerTest PROPERTIES TIMEOUT 10)

    if(NOT STURDY_BUILD_SHARED_LIBS)
        add_executable(
            WindowManagerGLFWUnusedProbe
            "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp"
        )
        target_link_libraries(WindowManagerGLFWUnusedProbe PRIVATE Sturdy::WindowManagerGLFW)
        sturdy_enable_warnings(WindowManagerGLFWUnusedProbe)
        add_test(
            NAME WindowManagerGLFWUnusedTest
            COMMAND "${CMAKE_COMMAND}"
                "-DSTURDY_STATIC_PROBE=$<TARGET_FILE:WindowManagerGLFWUnusedProbe>"
                "-DSTURDY_STATIC_AGGREGATE_LABEL=Sturdy::WindowManagerGLFW"
                -P "${CMAKE_SOURCE_DIR}/cmake/tests/AssertStaticAggregateUnused.cmake"
        )
        set_tests_properties(WindowManagerGLFWUnusedTest PROPERTIES TIMEOUT 60)

        add_executable(GlfwDefaultApplicationProbe tests/DefaultApplicationProbe.cpp)
        target_link_libraries(
            GlfwDefaultApplicationProbe
            PRIVATE
                Sturdy::Engine
                Sturdy::WindowManagerGLFW
        )
        sturdy_enable_warnings(GlfwDefaultApplicationProbe)
        add_test(
            NAME GlfwDefaultApplicationPayForUseTest
            COMMAND "${CMAKE_COMMAND}"
                "-DSTURDY_PROBE=$<TARGET_FILE:GlfwDefaultApplicationProbe>"
                "-DSTURDY_FORBIDDEN_PATTERN=glfw"
                "-DSTURDY_PROBE_LABEL=default Engine::Application"
                -P "${CMAKE_SOURCE_DIR}/cmake/tests/AssertNoBinaryFootprint.cmake"
        )
        set_tests_properties(GlfwDefaultApplicationPayForUseTest PROPERTIES TIMEOUT 10)
    endif()
endif()
