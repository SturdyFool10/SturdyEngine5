# SFT_REFLECT_INVOKE/SFT_REFLECT_FIRE_EVENT (Invoke.hpp) are gated by the preprocessor macro
# STURDY_REFLECTION_ENABLE_MODDING, which Invoke.hpp itself defaults to 1 (`#ifndef`) when nobody
# sets it. This is deliberately a per-consumer choice, not a whole-library build switch: a single
# Reflection.a is shared by every target, but any individual game target can opt out on its own
# via `target_compile_definitions(MyGame PRIVATE STURDY_REFLECTION_ENABLE_MODDING=0)` — with that
# set, the two macros degrade to a literal direct call/argument-evaluating no-op with no
# TypeRegistry lookup, no atomic load, nothing reflection-related instantiated at all (not merely
# "override always null"). SFT_REFLECT_TYPE/FIELD/METHOD/EVENT declarations and TypeRegistry
# itself are unaffected either way, so a game can still use reflection for its own internal
# introspection (an inspector, a generic serializer, ...) with modding compiled out.
sturdy_add_package(Reflection
    ENGINE_OPTIMIZED

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Tracy
)

# See Foundation/CMakeLists.txt's matching comment: a hot-reloadable game-logic module
# (Engine::HotReloadableModule) is always a real shared library and links Reflection in, so its
# objects must be PIC regardless of how the rest of the build is configured.
set_target_properties(Reflection PROPERTIES POSITION_INDEPENDENT_CODE ON)

sturdy_add_tests(Reflection TIMEOUT 20 EXCLUDE ModdingDisabledTest)

# Proves the modding-OFF path is functionally a plain call, not just "override always null" —
# built as its own target so it can override STURDY_REFLECTION_ENABLE_MODDING independently of
# the rest of this package (which stays ON for every other test in this directory).
sturdy_add_test(Reflection tests/ModdingDisabledTest.cpp TIMEOUT 20
    DEFINES STURDY_REFLECTION_ENABLE_MODDING=0)
