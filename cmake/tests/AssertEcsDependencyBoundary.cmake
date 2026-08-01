cmake_minimum_required(VERSION 3.28)

# Source-text boundary scan, mirroring AssertBaseDependencyBoundary.cmake's Box3D check: valid here
# because none of these dependencies may EVER legitimately appear in Ecs/CMakeLists.txt (unlike
# GLFW/Runtime-demo, which are correctly mentioned under their own if() guards elsewhere — see
# AssertFreshConfigureBoundary.cmake for that different case). The architecture plan's dependency-
# direction law is explicit: "EngineCore/runtime kernel does not depend on windowing, graphics, UI,
# physics implementations, or asset codecs" — Ecs is the innermost layer that law applies to, and
# Ecs/src/System.hpp's own runtime work (ExecutorPolicy::Synchronous) already proves a headless ECS
# consumer pays no Async worker-thread cost; this proves the build graph matches that promise by
# never giving a plain Foundation+Async ECS consumer a reason to link graphics/platform code at all.
if(NOT DEFINED STURDY_SOURCE_DIR)
    message(FATAL_ERROR
        "STURDY_SOURCE_DIR must point at the SturdyEngine source root."
    )
endif()

set(_sturdy_ecs_boundary_file "${STURDY_SOURCE_DIR}/Ecs/CMakeLists.txt")
if(NOT EXISTS "${_sturdy_ecs_boundary_file}")
    message(FATAL_ERROR
        "Ecs dependency boundary input does not exist:\n"
        "  ${_sturdy_ecs_boundary_file}"
    )
endif()

# Matched as "sturdy::<name>" (the actual PUBLIC_DEPS/PRIVATE_DEPS token shape used throughout this
# repo's CMake, e.g. "Sturdy::box3d", "Sturdy::UI" in Engine/CMakeLists.txt), not bare words — a bare
# "ui"/"core"/"renderer" substring check is too fragile against ordinary CMake vocabulary (this exact
# script's first draft false-positived on "ui" inside "BUILD_TESTING").
set(_sturdy_ecs_forbidden_dependencies
    sturdy::renderer
    sturdy::platform
    sturdy::rhi
    sturdy::core
    sturdy::ui
    sturdy::box3d
    sturdy::graphicsplatform
    sturdy::slang
    sturdy::vulkan
    sturdy::sdl3
    sturdy::glfw
)

file(READ "${_sturdy_ecs_boundary_file}" _dependency_contents)
string(TOLOWER "${_dependency_contents}" _dependency_contents_lower)

foreach(_forbidden IN LISTS _sturdy_ecs_forbidden_dependencies)
    string(FIND "${_dependency_contents_lower}" "${_forbidden}" _forbidden_index)
    if(NOT _forbidden_index EQUAL -1)
        message(FATAL_ERROR
            "Ecs/CMakeLists.txt references '${_forbidden}'. The ECS core package must stay a leaf "
            "over Foundation+Async only — graphics/windowing/physics dependencies belong in a "
            "consumer package (Engine, a future PhysicsBox3D adapter, ...), never in Ecs itself."
        )
    endif()
endforeach()

message(STATUS
    "Ecs/CMakeLists.txt stays a Foundation+Async leaf: no graphics/windowing/physics dependency reference found."
)
