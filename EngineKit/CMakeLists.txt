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
sturdy_add_package(EngineKit
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Async
        Sturdy::Ecs
        Sturdy::Core
        Sturdy::Renderer
        Sturdy::Engine
        Sturdy::Runtime
        Sturdy::GLM

    DEBUG_PUBLIC_DEPS
)

get_target_property(_sturdy_engine_kit_type EngineKit TYPE)
if(NOT _sturdy_engine_kit_type STREQUAL "INTERFACE_LIBRARY")
    message(FATAL_ERROR "EngineKit must remain an INTERFACE convenience aggregate.")
endif()
get_target_property(_sturdy_engine_kit_dependencies EngineKit INTERFACE_LINK_LIBRARIES)
foreach(_sturdy_engine_kit_required_dependency IN ITEMS
    Sturdy::Foundation
    Sturdy::Async
    Sturdy::Ecs
    Sturdy::Core
    Sturdy::Renderer
    Sturdy::Engine
    Sturdy::Runtime
    Sturdy::GLM
)
    if(NOT _sturdy_engine_kit_required_dependency IN_LIST _sturdy_engine_kit_dependencies)
        message(FATAL_ERROR
            "EngineKit is missing required interface dependency: "
            "${_sturdy_engine_kit_required_dependency}"
        )
    endif()
endforeach()

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING AND NOT STURDY_OS STREQUAL "Web")
    add_executable(EngineKitAccessTest tests/EngineKitAccessTest.cpp)
    target_link_libraries(EngineKitAccessTest PRIVATE Sturdy::EngineKit)
    sturdy_enable_warnings(EngineKitAccessTest)
    add_test(NAME EngineKitAccessTest COMMAND EngineKitAccessTest)
    set_tests_properties(EngineKitAccessTest PROPERTIES TIMEOUT 10)

    if(NOT STURDY_BUILD_SHARED_LIBS)
        add_executable(
            EngineKitStaticUnusedProbe
            "${CMAKE_SOURCE_DIR}/cmake/tests/StaticAggregateUnused.cpp"
        )
        target_link_libraries(EngineKitStaticUnusedProbe PRIVATE Sturdy::EngineKit)
        sturdy_enable_warnings(EngineKitStaticUnusedProbe)
        add_test(
            NAME EngineKitStaticUnusedTest
            COMMAND "${CMAKE_COMMAND}"
                "-DSTURDY_STATIC_PROBE=$<TARGET_FILE:EngineKitStaticUnusedProbe>"
                "-DSTURDY_STATIC_AGGREGATE_LABEL=Sturdy::EngineKit"
                -P "${CMAKE_SOURCE_DIR}/cmake/tests/AssertStaticAggregateUnused.cmake"
        )
        set_tests_properties(EngineKitStaticUnusedTest PROPERTIES TIMEOUT 10)
    endif()
endif()
