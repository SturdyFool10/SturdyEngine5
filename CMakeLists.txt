cmake_minimum_required(VERSION 3.28)

if(NOT CMAKE_GENERATOR MATCHES "^Ninja")
    message(FATAL_ERROR
        "SturdyEngine5 uses C++ module file sets and requires the Ninja generator for "
        "module dependency scanning plus compile_commands.json export. Configure with "
        "one of the presets in CMakePresets.json."
    )
endif()

project(SturdyEngine5
    VERSION 0.1.0
    DESCRIPTION "Sturdy Engine 5"
    LANGUAGES C CXX
)

include(CTest)

set(STURDY_CPP_STANDARD 26 CACHE STRING "C++ standard used by SturdyEngine targets. 26 is used ahead of ratification for the newest available toolchain features.")
set(CMAKE_CXX_STANDARD ${STURDY_CPP_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# std::float_denorm_style and numeric_limits::has_denorm(_loss) are deprecated (not yet removed) as of
# C++23; Foundation/WideTraits.hpp's numeric_limits specializations for the wide numeric types still
# populate them for compatibility with existing generic code. Must be set here rather than in that
# header: the deprecation attribute attaches to the enum's declaration the first time any translation
# unit includes <limits>, so a leaf header's #define is too late once something upstream of it has
# already pulled <limits> in.
add_compile_definitions(_SILENCE_CXX23_DENORM_DEPRECATION_WARNING)

# Forced empty and set as early as possible, before any dependency is fetched: several fetched
# projects (freetype, OpenEXR, ...) set this themselves internally to suffix Debug-config library
# filenames (e.g. "Imath-3_2_d.lib"), each guarded with its own "if (NOT DEFINED
# CMAKE_DEBUG_POSTFIX)" check. Once any one of them sets it, every OTHER dependency processed
# afterward sees it as already-defined and inherits that same suffix on its own output filenames --
# but only for targets whose OWN CMakeLists.txt actually consults the variable when creating the
# target, which was already true for most of them and NOT true for whichever specific new target
# tripped this (empirically: adding OpenEXR was what finally surfaced it, as a hard link failure --
# "Imath-3_2_d.lib" referenced by one generated ninja rule but never actually produced, because a
# different rule for the same logical target computed its filename before the suffix propagated).
# Forcing this to empty here, before any of that runs, keeps every fetched dependency's own
# defensive "if (NOT DEFINED ...)" check finding it already decided and moving on.
set(CMAKE_DEBUG_POSTFIX "" CACHE STRING "Suffix appended to library filenames in Debug configs." FORCE)

# Same shape of problem, same fix, different variables: openjpeg's own CMakeLists.txt does
#     set(EXECUTABLE_OUTPUT_PATH ${OPENJPEG_BINARY_DIR}/bin CACHE PATH "...")
# (and the same for LIBRARY_OUTPUT_PATH). Those are CMake's *legacy* global output-path variables,
# not the modern per-target CMAKE_RUNTIME_OUTPUT_DIRECTORY, so they retarget the output location of
# every target in this entire project -- engine libraries, demos, and every test executable -- into
# openjpeg's own build directory. The visible symptom is the whole CTest suite failing as "Not Run"
# / "does not exist", because each test's registered command still points at the path the target
# was declared with. Seeding both as empty cache entries here makes openjpeg's own non-FORCE
# set(... CACHE ...) calls find them already present and leave them alone.
set(EXECUTABLE_OUTPUT_PATH "" CACHE PATH "Legacy global executable output path; deliberately unset." FORCE)
set(LIBRARY_OUTPUT_PATH "" CACHE PATH "Legacy global library output path; deliberately unset." FORCE)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(STURDY_FETCH_DEPENDENCIES "Download third-party dependencies with FetchContent when packages are not already available." ON)
# Off by default: KhronosGroup/glTF-Sample-Assets is a large, ever-growing asset repo, not a build
# dependency — only needed when exercising the glTF import path (plans/gltf-import.md) against real
# content. Turn on to let the external RuntimeDemoGameLogic package find real sample models via
# FetchContent's usual
# shared-cache mechanism, same as any other STURDY_FETCH_DEPENDENCIES source.
option(STURDY_FETCH_SAMPLE_ASSETS "Fetch KhronosGroup/glTF-Sample-Assets for local glTF import testing." OFF)
option(
    STURDY_BUILD_RUNTIME_DEMO
    "Build the external Runtime demo logic and executable packages."
    ON
)
option(
    STURDY_BUILD_GLFW_WINDOW_PROVIDER
    "Build the optional GLFW desktop window provider alongside the default SDL3 provider."
    OFF
)
option(STURDY_BUILD_SHARED_LIBS "Build SturdyEngine targets as shared libraries." OFF)
option(
    STURDY_ENABLE_STATIC_DEAD_STRIPPING
    "Compile static Sturdy targets into dead-strippable sections and propagate linker GC."
    ON
)
option(STURDY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors for SturdyEngine targets." OFF)
# Controls Tracy's own TRACY_ENABLE (see sturdy_fetch_tracy() in cmake/SturdyDependencies.cmake).
# The Tracy 0.13.1 client library is always fetched and linked so instrumented source always
# compiles; this only decides whether its zone/frame macros record anything or are no-ops. Turn
# ON to capture with Tracy Profiler 0.13.1, and OFF for Dist (shipping) builds that should carry
# zero profiling overhead.
option(STURDY_ENABLE_TRACY "Compile Tracy 0.13.1 profiler instrumentation into Sturdy targets." OFF)
option(STURDY_USE_COMPILER_LAUNCHER "Use sccache/ccache when available to speed repeated C/C++ builds." ON)
set(STURDY_COMPILER_LAUNCHER "" CACHE FILEPATH "Explicit compiler launcher executable. Overrides auto-detected sccache/ccache when set.")
option(STURDY_GLOB_CONFIGURE_DEPENDS "Re-check source globs during every build. Disable for faster incremental Ninja builds; re-run CMake after adding/removing files." OFF)
option(STURDY_INCLUDE_HEADERS_IN_TARGETS "Add headers to generated target source lists for IDE visibility. Disable for faster configure/generate." OFF)
option(STURDY_FETCH_UPDATES_DISCONNECTED "Skip FetchContent git update checks after dependencies are populated." ON)
# Force the Windows GUI (WinMain, no console) entry point in every configuration. When OFF, WinMain
# is used only for Windows Dist builds; Debug/RelWithDebInfo/Release keep the console main().
option(SFT_USE_WINMAIN "Always use the Windows WinMain entry point, regardless of build type." OFF)

# Builds the WebGPU (Dawn) RHI backend. Off by default purely because of what it costs to build:
# Dawn brings its own copies of Abseil, SPIRV-Tools, and Tint, which roughly doubles a clean
# configure-and-build. The backend itself is complete when enabled; turning this off only removes
# it from the backend registry, leaving Vulkan/D3D12 exactly as they were.
option(STURDY_ENABLE_WEBGPU "Build the WebGPU (Dawn) RHI backend." OFF)
# Off by default: registers FfiNativeHandleProbe (see FFI/CMakeLists.txt) as real CTest cases
# instead of a manual-only tool. It creates a real window and GPU device, which a headless CI
# runner does not have — turn this ON on a developer machine with a GPU and a display to get
# automatic pass/fail coverage of the RHI/pipeline paths CTest otherwise never exercises.
option(STURDY_ENABLE_GPU_TESTS "Register GPU-backed FFI probe tests with CTest (requires a real GPU and display)." OFF)

if(NOT STURDY_BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build third-party dependencies as shared libraries." FORCE)
endif()

if(STURDY_FETCH_UPDATES_DISCONNECTED)
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Skip FetchContent update checks" FORCE)
endif()

if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND CMAKE_CONFIGURATION_TYPES Dist)
    list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations." FORCE)
endif()

set(CMAKE_C_FLAGS_DIST "${CMAKE_C_FLAGS_RELEASE}" CACHE STRING "C flags for Dist builds." FORCE)
set(CMAKE_CXX_FLAGS_DIST "${CMAKE_CXX_FLAGS_RELEASE}" CACHE STRING "C++ flags for Dist builds." FORCE)
set(CMAKE_EXE_LINKER_FLAGS_DIST "${CMAKE_EXE_LINKER_FLAGS_RELEASE}" CACHE STRING "Executable linker flags for Dist builds." FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_DIST "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}" CACHE STRING "Shared library linker flags for Dist builds." FORCE)
set(CMAKE_STATIC_LINKER_FLAGS_DIST "${CMAKE_STATIC_LINKER_FLAGS_RELEASE}" CACHE STRING "Static library linker flags for Dist builds." FORCE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Arch/OS/Profile build matrix (validates STURDY_ARCH/STURDY_OS, applies cross --target flags)
# and build-throughput settings (ThinLTO for optimized profiles, lld, split DWARF). Both must
# run before dependencies and subdirectories so every target inherits them.
include(SturdyMatrix)
sturdy_apply_matrix()

set(
    STURDY_STATIC_ARCHIVE_TIMEOUT_SECONDS
    120
    CACHE STRING
    "Maximum duration of a guarded Windows or Linux static-library archive step; 0 disables the guard."
)
if(NOT STURDY_STATIC_ARCHIVE_TIMEOUT_SECONDS MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "STURDY_STATIC_ARCHIVE_TIMEOUT_SECONDS must be a non-negative integer."
    )
endif()

if(STURDY_BUILD_GLFW_WINDOW_PROVIDER AND STURDY_OS STREQUAL "Web")
    message(FATAL_ERROR
        "STURDY_BUILD_GLFW_WINDOW_PROVIDER is unavailable for Web builds."
    )
endif()

if(STURDY_FETCH_SAMPLE_ASSETS AND NOT STURDY_BUILD_RUNTIME_DEMO)
    message(FATAL_ERROR
        "STURDY_FETCH_SAMPLE_ASSETS requires STURDY_BUILD_RUNTIME_DEMO."
    )
endif()

include(SturdyBuildSpeed)

function(sturdy_configure_compiler_launcher)
    if(NOT STURDY_USE_COMPILER_LAUNCHER)
        return()
    endif()

    if(STURDY_COMPILER_LAUNCHER)
        set(_sturdy_compiler_launcher "${STURDY_COMPILER_LAUNCHER}")
    else()
        find_program(_sturdy_compiler_launcher NAMES sccache ccache)
    endif()

    if(_sturdy_compiler_launcher)
        set(CMAKE_C_COMPILER_LAUNCHER "${_sturdy_compiler_launcher}" CACHE FILEPATH "C compiler launcher" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_sturdy_compiler_launcher}" CACHE FILEPATH "C++ compiler launcher" FORCE)
        message(STATUS "Compiler launcher: ${_sturdy_compiler_launcher}")
    endif()
endfunction()

sturdy_configure_compiler_launcher()

include(SturdyDependencies)
include(SturdyLicenses)
include(SturdyShaders)
include(SturdyPackage)

# The reusable engine stack is performance-sensitive infrastructure. Product layers above it
# (Runtime, hosts, demos, and tests) retain the requested profile for normal debugging, while the
# engine and every first-party library it directly builds on compile as RelWithDebInfo unless the
# whole build is Dist. SturdyPackage.cmake consumes this list when it creates each target.
set(STURDY_ENGINE_OPTIMIZED_TARGETS
    Foundation
    Async
    Ecs
    WindowManager
    WindowManagerGLFW
    RHI
    Core
    Renderer
    Engine
)

sturdy_configure_dependencies()
# Must run before add_subdirectory(Core) below, since Core/CMakeLists.txt's sturdy_add_package()
# call consumes STURDY_SHADERS_EMBED_CPP as one of its SOURCES.
sturdy_generate_shader_embeds()
add_subdirectory(Foundation)
add_subdirectory(Async)
add_subdirectory(Ecs)
add_subdirectory(WindowManager)
add_subdirectory(RHI)
add_subdirectory(Core)
add_subdirectory(Renderer)
add_subdirectory(Engine)
if(STURDY_BUILD_GLFW_WINDOW_PROVIDER)
    add_subdirectory(WindowManager/Providers/GLFW)
endif()
# Reusable host infrastructure is always available. Demonstration content and executable composition
# live in separate top-level packages and are the only targets gated by STURDY_BUILD_RUNTIME_DEMO.
add_subdirectory(ApplicationHost)
add_subdirectory(Runtime)
# Base convenience aggregate (Foundation+Async+Ecs+Core+Renderer+Engine+Runtime+GLM) for a normal
# graphical/game application — see EngineKit/CMakeLists.txt. Always available for the same reason
# Runtime itself is: it is reusable infrastructure, not demo content.
add_subdirectory(EngineKit)
# Stable C ABI for non-C++ consumers. A leaf on top of Runtime, like EngineKit: reusable
# infrastructure rather than demo content, so it is always available.
add_subdirectory(FFI)
if(STURDY_BUILD_RUNTIME_DEMO)
    add_subdirectory(Demos)
endif()

# Pay-for-use regression coverage for the root-level opt-out options above: each disables a whole
# dependency/subdirectory, so (unlike Box3D's AssertBaseDependencyBoundary.cmake source-text scan,
# which works because Box3D must never appear anywhere) the only real proof the option works is a
# fresh configure with it off, checked against the resulting build graph — see
# AssertFreshConfigureBoundary.cmake's own header comment. These were previously verified by hand in
# a throwaway build tree and never turned into a persistent regression test.
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    set(
        _sturdy_fresh_configure_boundary_test
        "${CMAKE_SOURCE_DIR}/cmake/tests/AssertFreshConfigureBoundary.cmake"
    )

    add_test(
        NAME GlfwProviderOffDependencyBoundaryTest
        COMMAND "${CMAKE_COMMAND}"
            "-DSTURDY_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DSTURDY_PROBE_LABEL=glfw-off"
            "-DSTURDY_PROBE_OPTION=STURDY_BUILD_GLFW_WINDOW_PROVIDER=OFF"
            "-DSTURDY_PROBE_FORBIDDEN_STRING=glfw"
            "-DSTURDY_PROBE_ARCH=${STURDY_ARCH}"
            "-DSTURDY_PROBE_OS=${STURDY_OS}"
            "-DSTURDY_PROBE_COMPILER=${STURDY_COMPILER}"
            "-DSTURDY_PROBE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DSTURDY_PROBE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            -P "${_sturdy_fresh_configure_boundary_test}"
    )
    set_tests_properties(GlfwProviderOffDependencyBoundaryTest PROPERTIES TIMEOUT 300)

    add_test(
        NAME RuntimeDemoOffDependencyBoundaryTest
        COMMAND "${CMAKE_COMMAND}"
            "-DSTURDY_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DSTURDY_PROBE_LABEL=runtime-demo-off"
            "-DSTURDY_PROBE_OPTION=STURDY_BUILD_RUNTIME_DEMO=OFF"
            "-DSTURDY_PROBE_FORBIDDEN_STRING=RuntimeDemo"
            "-DSTURDY_PROBE_ARCH=${STURDY_ARCH}"
            "-DSTURDY_PROBE_OS=${STURDY_OS}"
            "-DSTURDY_PROBE_COMPILER=${STURDY_COMPILER}"
            "-DSTURDY_PROBE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DSTURDY_PROBE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            -P "${_sturdy_fresh_configure_boundary_test}"
    )
    set_tests_properties(RuntimeDemoOffDependencyBoundaryTest PROPERTIES TIMEOUT 300)
endif()
