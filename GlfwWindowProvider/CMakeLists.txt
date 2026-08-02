# Optional GLFW window provider. This archive is deliberately separate from Sturdy::Platform:
# configuring it makes the provider available, while a product pays its link/runtime footprint only
# by explicitly referencing GLFW::create_window and linking Sturdy::GlfwWindowProvider.
set(_sturdy_glfw_provider_platform_dirs Linux MacOS Windows)
if(STURDY_OS STREQUAL "Windows")
    set(_sturdy_glfw_provider_active_dir Windows)
elseif(STURDY_OS STREQUAL "MacOS")
    set(_sturdy_glfw_provider_active_dir MacOS)
else()
    # Linux and FreeBSD share the POSIX provider source view, matching Platform's source selection.
    set(_sturdy_glfw_provider_active_dir Linux)
endif()
set(_sturdy_glfw_provider_excluded_dirs ${_sturdy_glfw_provider_platform_dirs})
list(REMOVE_ITEM _sturdy_glfw_provider_excluded_dirs "${_sturdy_glfw_provider_active_dir}")

sturdy_add_package(GlfwWindowProvider
    EXCLUDE_SOURCE_DIRS
        tests
        ${_sturdy_glfw_provider_excluded_dirs}

    PUBLIC_DEPS
        Sturdy::Platform
        # The concrete GLFWWindow API exposes GLFW types. This dependency is scoped to the optional
        # provider target; base Platform/Engine/Runtime consumers never inherit it.
        Sturdy::GLFW

    DEBUG_PUBLIC_DEPS
)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(GlfwWindowProviderFactoryAccessTest tests/FactoryAccessTest.cpp)
    target_link_libraries(GlfwWindowProviderFactoryAccessTest PRIVATE Sturdy::GlfwWindowProvider)
    sturdy_enable_warnings(GlfwWindowProviderFactoryAccessTest)
    add_test(NAME GlfwWindowProviderFactoryAccessTest COMMAND GlfwWindowProviderFactoryAccessTest)
    set_tests_properties(GlfwWindowProviderFactoryAccessTest PROPERTIES TIMEOUT 10)

    add_executable(GlfwWindowEventAndManagerTest tests/WindowEventAndManagerTest.cpp)
    target_link_libraries(GlfwWindowEventAndManagerTest PRIVATE Sturdy::GlfwWindowProvider)
    sturdy_enable_warnings(GlfwWindowEventAndManagerTest)
    add_test(NAME GlfwWindowEventAndManagerTest COMMAND GlfwWindowEventAndManagerTest)
    set_tests_properties(GlfwWindowEventAndManagerTest PROPERTIES TIMEOUT 10)

    if(NOT STURDY_BUILD_SHARED_LIBS)
        add_executable(
            GlfwWindowProviderUnusedProbe
            "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp"
        )
        target_link_libraries(GlfwWindowProviderUnusedProbe PRIVATE Sturdy::GlfwWindowProvider)
        sturdy_enable_warnings(GlfwWindowProviderUnusedProbe)
        add_test(
            NAME GlfwWindowProviderUnusedTest
            COMMAND "${CMAKE_COMMAND}"
                "-DSTURDY_STATIC_PROBE=$<TARGET_FILE:GlfwWindowProviderUnusedProbe>"
                "-DSTURDY_STATIC_AGGREGATE_LABEL=Sturdy::GlfwWindowProvider"
                -P "${CMAKE_SOURCE_DIR}/cmake/tests/AssertStaticAggregateUnused.cmake"
        )
        set_tests_properties(GlfwWindowProviderUnusedTest PROPERTIES TIMEOUT 10)

        add_executable(GlfwDefaultApplicationProbe tests/DefaultApplicationProbe.cpp)
        target_link_libraries(
            GlfwDefaultApplicationProbe
            PRIVATE
                Sturdy::Engine
                Sturdy::GlfwWindowProvider
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
