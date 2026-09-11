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
    EXCLUDE_SOURCE_DIRS
        tests

    PUBLIC_DEPS
        Sturdy::Foundation
        Sturdy::Tracy
)

# See Foundation/CMakeLists.txt's matching comment: a hot-reloadable game-logic module
# (Engine::HotReloadableModule) is always a real shared library and links Reflection in, so its
# objects must be PIC regardless of how the rest of the build is configured.
set_target_properties(Reflection PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(ReflectionTypeRegistryTest tests/TypeRegistryTest.cpp)
    target_link_libraries(ReflectionTypeRegistryTest PRIVATE Sturdy::Reflection)
    sturdy_enable_warnings(ReflectionTypeRegistryTest)
    add_test(NAME ReflectionTypeRegistryTest COMMAND ReflectionTypeRegistryTest)
    set_tests_properties(ReflectionTypeRegistryTest PROPERTIES TIMEOUT 20)

    # Proves the modding-OFF path is functionally a plain call, not just "override always null" —
    # built as its own target so it can override STURDY_REFLECTION_ENABLE_MODDING independently of
    # the rest of this package (which stays ON for every other test in this directory).
    add_executable(ReflectionModdingDisabledTest tests/ModdingDisabledTest.cpp)
    target_link_libraries(ReflectionModdingDisabledTest PRIVATE Sturdy::Reflection)
    target_compile_definitions(ReflectionModdingDisabledTest PRIVATE STURDY_REFLECTION_ENABLE_MODDING=0)
    sturdy_enable_warnings(ReflectionModdingDisabledTest)
    add_test(NAME ReflectionModdingDisabledTest COMMAND ReflectionModdingDisabledTest)
    set_tests_properties(ReflectionModdingDisabledTest PROPERTIES TIMEOUT 20)

    add_executable(ReflectionSerializeTest tests/SerializeTest.cpp)
    target_link_libraries(ReflectionSerializeTest PRIVATE Sturdy::Reflection)
    sturdy_enable_warnings(ReflectionSerializeTest)
    add_test(NAME ReflectionSerializeTest COMMAND ReflectionSerializeTest)
    set_tests_properties(ReflectionSerializeTest PROPERTIES TIMEOUT 20)

    add_executable(ReflectionStaticEnumConstructorTest tests/StaticEnumConstructorTest.cpp)
    target_link_libraries(ReflectionStaticEnumConstructorTest PRIVATE Sturdy::Reflection)
    sturdy_enable_warnings(ReflectionStaticEnumConstructorTest)
    add_test(NAME ReflectionStaticEnumConstructorTest COMMAND ReflectionStaticEnumConstructorTest)
    set_tests_properties(ReflectionStaticEnumConstructorTest PROPERTIES TIMEOUT 20)

    add_executable(ReflectionOptionalTest tests/OptionalTest.cpp)
    target_link_libraries(ReflectionOptionalTest PRIVATE Sturdy::Reflection)
    sturdy_enable_warnings(ReflectionOptionalTest)
    add_test(NAME ReflectionOptionalTest COMMAND ReflectionOptionalTest)
    set_tests_properties(ReflectionOptionalTest PROPERTIES TIMEOUT 20)

    add_executable(ReflectionDocumentTest tests/DocumentTest.cpp)
    target_link_libraries(ReflectionDocumentTest PRIVATE Sturdy::Reflection)
    sturdy_enable_warnings(ReflectionDocumentTest)
    add_test(NAME ReflectionDocumentTest COMMAND ReflectionDocumentTest)
    set_tests_properties(ReflectionDocumentTest PROPERTIES TIMEOUT 20)
endif()
