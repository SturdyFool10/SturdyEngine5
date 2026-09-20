# Sturdy.EngineKit — the base convenience aggregate for a normal graphical/game application:
# Foundation, Async (the task-spawning system is core, not optional, for anything using this kit),
# Ecs (likewise core — any app using this kit gets the entity/component/schedule system, not just
# the rendering half), Core, Renderer, Engine, and Runtime, plus direct access to the lower-level
# dependencies engine code actually programs against (GLM today; add others here only as a real
# second consumer needs them).
#
# This is an `INTERFACE` target and a convenience preset, not privileged architecture (matches
# plans/generalized-composable-engine-architecture.md's "Convenience aggregates" section) — it exists
# so a normal product links one thing instead of separately reasoning about transitive PUBLIC_DEPS
# chains. Every one of these was already reachable by linking Sturdy::Runtime alone (GLM included —
# Core and Renderer already list it PUBLIC); EngineKit only makes that bundle a single named target.
# A consumer with narrower needs (e.g. Tier 0/1/2 from the architecture doc) should keep linking the
# specific packages it actually needs instead of this kit.
set(_sturdy_engine_kit_dependencies
    Sturdy::Foundation
    Sturdy::Async
    Sturdy::Ecs
    Sturdy::Core
    Sturdy::Renderer
    Sturdy::Engine
    Sturdy::Runtime
    Sturdy::GLM
)

sturdy_add_package(EngineKit
    PUBLIC_DEPS
        ${_sturdy_engine_kit_dependencies}
)

get_target_property(_sturdy_engine_kit_type EngineKit TYPE)
if(NOT _sturdy_engine_kit_type STREQUAL "INTERFACE_LIBRARY")
    message(FATAL_ERROR "EngineKit must remain an INTERFACE convenience aggregate.")
endif()

sturdy_add_tests(EngineKit TIMEOUT 60)
sturdy_add_static_unused_test(EngineKit NAME_STEM EngineKitStaticUnused)
