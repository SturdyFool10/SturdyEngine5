#pragma once

#include <Ecs/World.hpp>
#include <Reflection/Reflection.hpp>

#include <Foundation/Foundation.hpp>

#include <expected>
#include <optional>
#include <string_view>
#include <utility>

namespace SFT::Engine {

    /// `Ecs::ComponentRegistry` and `Reflection::TypeRegistry` are deliberately two separate
    /// systems — `Ecs` stays a Foundation+Async leaf (see `AssertEcsDependencyBoundary.cmake`),
    /// and `Reflection` stays independent of `Ecs` (a component is one kind of reflectable type,
    /// not the only one). This file is the bridge between them, living here in `Engine` — which
    /// already depends on both — rather than in either.
    enum class EcsReflectionBridgeErrorCode : u32 {
        /// `T`/the component id could not be registered with the `ComponentRegistry`.
        ComponentNotRegistered,
        /// The `Reflection::TypeInfo`'s `size`/`align` disagree with the `Ecs::ComponentInfo`'s —
        /// almost always a `SFT_ECS_COMPONENT`/`SFT_REFLECT_TYPE` declared against drifted field
        /// sets for the same C++ type.
        DescriptorMismatch,
        /// No field with that name is declared (or inherited) on the given `TypeInfo`.
        FieldNotFound,
        /// `World::read_component_erased`/`write_component_erased` failed; see `world_error`.
        WorldAccessFailed,
        /// The field's declared size doesn't match what the caller supplied, or the field has no
        /// readable/writable representation (`FieldInfo::copy_get`/`copy_set`, or `ReadOnly`).
        FieldAccessFailed,
    };

    struct EcsReflectionBridgeError {
        EcsReflectionBridgeErrorCode code = EcsReflectionBridgeErrorCode::ComponentNotRegistered;
        UString message;
        /// Engaged only when `code == WorldAccessFailed`.
        std::optional<Ecs::WorldErasedErrorCode> world_error;
    };

    template <class Value>
    using EcsReflectionBridgeExpected = std::expected<Value, EcsReflectionBridgeError>;

    /// Registers `T` with both `components` and the process-wide `Reflection::TypeRegistry`,
    /// cross-checking that their `size`/`align` agree.
    ///
    /// Both registries already do their own on-first-use lazy registration (`try_register<T>`);
    /// this exists only to catch a real, otherwise-silent failure mode: `ComponentRegistry`'s
    /// `try_register`/`register_component` will keep an already-registered `ComponentInfo` under
    /// a matching canonical name even when it no longer agrees with what `sizeof(T)`/`alignof(T)`
    /// would produce today (e.g. it was registered by hand, or by a prior build with a different
    /// layout) — silently keeping a stale size on record rather than erroring. Cross-checking it
    /// here, once, before trusting it is exactly what protects `read_component_field`/
    /// `write_component_field` below from sizing their scratch buffer off a stale `size` and
    /// corrupting memory the first time a mod touches that component.
    ///
    /// @return Returns the component id and reflection key on success.
    template <class T>
    [[nodiscard]] EcsReflectionBridgeExpected<std::pair<Ecs::ComponentId, Reflection::TypeId>> ensure_reflected(Ecs::ComponentRegistry &components) {
        auto component_id = components.try_register<T>();
        if (!component_id) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::ComponentNotRegistered,
                .message = component_id.error().message,
                .world_error = std::nullopt,
            });
        }
        const Reflection::TypeInfo &type = Reflection::TypeRegistry::instance().type<T>();
        const Ecs::ComponentInfo *component_info = components.info(*component_id);
        if (component_info == nullptr || component_info->size != type.size || component_info->align != type.align) {
            return std::unexpected(EcsReflectionBridgeError{
                .code = EcsReflectionBridgeErrorCode::DescriptorMismatch,
                .message = UString{"SFT_ECS_COMPONENT and SFT_REFLECT_TYPE disagree on size/align for this type"},
                .world_error = std::nullopt,
            });
        }
        return std::pair{*component_id, type.key};
    }

    /// Reads one named field of `entity`'s `component`, going through `World`'s existing
    /// whole-component erased read path (copy out, extract the field, discard the rest) plus
    /// `Reflection`'s `FieldInfo` accessor — the "give me entity E's component C's field F by
    /// name" primitive neither system alone provides, and the one a mod actually wants.
    ///
    /// @note `type` must have already been registered for `component`'s C++ type (see
    /// `ensure_reflected`). Allocates a `type.size`-byte scratch buffer per call — this is not a
    /// hot-path operation; it is the deliberately-not-free mod/tooling-facing path, in exchange
    /// for `SFT_REFLECT_INVOKE`-style call sites elsewhere in the game staying free.
    [[nodiscard]] EcsReflectionBridgeExpected<void> read_component_field(Ecs::World &world,
                                                                         Ecs::Entity entity,
                                                                         Ecs::ComponentId component,
                                                                         const Reflection::TypeInfo &type,
                                                                         std::string_view field_name,
                                                                         void *out_data,
                                                                         usize size);

    /// Writes one named field of `entity`'s `component` (read-modify-write against the whole
    /// component through `World`'s erased API). See `read_component_field`.
    [[nodiscard]] EcsReflectionBridgeExpected<void> write_component_field(Ecs::World &world,
                                                                          Ecs::Entity entity,
                                                                          Ecs::ComponentId component,
                                                                          const Reflection::TypeInfo &type,
                                                                          std::string_view field_name,
                                                                          const void *in_data,
                                                                          usize size);

} // namespace SFT::Engine
