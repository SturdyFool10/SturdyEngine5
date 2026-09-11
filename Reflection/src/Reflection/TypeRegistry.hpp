#pragma once

#include <Reflection/Contract.hpp>
#include <Reflection/EnumInfo.hpp>
#include <Reflection/Macros.hpp>
#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <deque>
#include <expected>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <tracy/Tracy.hpp>

namespace SFT::Reflection {


    enum class TypeRegistryErrorCode : u32 {
        InvalidDescriptor,
        StableKeyCollision,
        CanonicalNameCollision,
    };

    struct TypeRegistryError {
        TypeRegistryErrorCode code = TypeRegistryErrorCode::InvalidDescriptor;
        UString message;
    };

    template <class Value>
    using TypeRegistryExpected = std::expected<Value, TypeRegistryError>;


    /// Process-wide runtime type registry. Unlike `Ecs::ComponentRegistry` (deliberately
    /// per-`World`, since component sets can legitimately differ between worlds), this is a
    /// singleton: a mod arriving through the FFI boundary has only a type name/`TypeId` and no
    /// world handle to resolve it against, so it needs one well-known global resolution point —
    /// the same shape as Java's `Class.forName`.
    class TypeRegistry {
      public:
        /// Returns the process-wide `TypeRegistry` instance.
        ///
        /// @return Returns a reference to the singleton instance.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static TypeRegistry &instance() noexcept;

        TypeRegistry(const TypeRegistry &) = delete;
        TypeRegistry &operator=(const TypeRegistry &) = delete;
        TypeRegistry(TypeRegistry &&) = delete;
        TypeRegistry &operator=(TypeRegistry &&) = delete;

        /// Registers a fully-built `TypeInfo`.
        ///
        /// @param info Description of the type to register.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why
        /// the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state.
        [[nodiscard]] TypeRegistryExpected<TypeId> register_type(TypeInfo info);

        /// Registers `T` on first use, matching `Ecs::ComponentRegistry::try_register`: the
        /// descriptor-build cost (`Detail::make_type_info<T>()`) is paid only for types a mod
        /// actually touches, and never through a static-init-order-dependent path.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why
        /// the operation failed.
        template <class T>
        [[nodiscard]] TypeRegistryExpected<TypeId> try_register() {
            ZoneScopedN("TypeRegistry::try_register");
            using TypeT = std::remove_cv_t<T>;
            constexpr std::string_view name = Detail::type_name<TypeT>();
            const TypeId key = TypeId::from_name(name);
            if (find(key) != nullptr) {
                return key;
            }
            // Cascade-register the reflected base first (if any), so `find_field`/`find_method`/
            // `find_event`'s runtime base-chain walk (which follows `TypeInfo::base_type` through
            // `find(TypeId)`, not through compile-time knowledge of `TypeTraits<T>::BaseType`)
            // never dead-ends at an ancestor nobody has asked for yet.
            if constexpr (requires { typename TypeTraits<TypeT>::BaseType; }) {
                auto base_registered = try_register<typename TypeTraits<TypeT>::BaseType>();
                if (!base_registered) {
                    return base_registered;
                }
            }
            return register_type(Detail::make_type_info<TypeT>());
        }

        /// Returns `T`'s `TypeInfo`, registering it on first use. Terminates the process on
        /// failure (mirrors `Ecs::ComponentRegistry::component`) — a reflected type that cannot
        /// register is a programming error, not a normal runtime condition.
        ///
        /// @return Returns a reference to the type's descriptor.
        template <class T>
        [[nodiscard]] const TypeInfo &type() {
            ZoneScopedN("TypeRegistry::type");
            using TypeT = std::remove_cv_t<T>;
            auto registered = try_register<TypeT>();
            if (!registered) {
                Detail::contract_violation(
                    "Reflection type resolution failed for '{}': {}",
                    Detail::type_name<TypeT>(),
                    registered.error().message);
            }
            const TypeInfo *found = find(*registered);
            if (found == nullptr) {
                Detail::contract_violation(
                    "Reflection type '{}' vanished immediately after registration.",
                    Detail::type_name<TypeT>());
            }
            return *found;
        }

        /// Unregisters a type, so no future `find`/`type<T>()`/`try_register<T>()` call can reach
        /// it — the mod-unload primitive.
        ///
        /// The `TypeInfo` itself is not physically freed (its slot stays alive in `infos_`
        /// forever, an accepted leak bounded by "how many distinct types were ever registered and
        /// unregistered over the process lifetime"): erasing it would invalidate every other
        /// `TypeInfo`/`FieldInfo`/`MethodInfo`/`EventInfo` pointer any caller might be mid-use
        /// with (`std::deque::erase` invalidates references to every element after the erased
        /// one), which would turn "unregister a mod" into a use-after-free hazard for every
        /// *other* mod instead of fixing one. Once nobody can `find()` it, its own function
        /// pointers (which may point into a shared library about to be unloaded) simply become
        /// unreachable dead weight instead of a dangling-call hazard.
        ///
        /// @note The caller must ensure no other thread is mid-call against this type (reading a
        /// field, invoking a method, iterating `for_each_type`) before actually unloading the
        /// code its function pointers point into — this function only stops *new* lookups from
        /// finding it, it cannot recall references already handed out. In practice this means:
        /// quiesce the mod (stop dispatching to it) before calling `unregister_type`, and only
        /// unload its shared library after that.
        ///
        /// @return Returns `true` when `key` was registered and is now unregistered; `false` when
        /// it was not found.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool unregister_type(TypeId key) noexcept;
        /// Same as the `TypeId` overload, looked up by name.
        [[nodiscard]] bool unregister_type(const ustr &canonical_name) noexcept;

        /// Finds the requested entry by `TypeId`.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const TypeInfo *find(TypeId key) const noexcept;
        /// Finds the requested entry by canonical name.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const TypeInfo *find(const ustr &canonical_name) const noexcept;

        /// Finds `field_key` on `type`, or — if not declared there — on the nearest reflected
        /// ancestor reachable through `TypeInfo::base_type`. The Java-`getField` analog (which
        /// also searches superclasses); `TypeInfo::find_field` is the declared-only equivalent.
        ///
        /// @return Returns a pointer to the requested field, or `nullptr` when it is unavailable
        /// on `type` or any reflected ancestor.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const FieldInfo *find_field(const TypeInfo &type, TypeId field_key) const noexcept;
        /// Same as the `TypeId` overload, looked up by name.
        [[nodiscard]] const FieldInfo *find_field(const TypeInfo &type, std::string_view field_name) const noexcept;
        /// Finds `method_key` on `type` or its nearest reflected ancestor. See `find_field`.
        ///
        /// @return Returns a pointer to the requested method, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MethodInfo *find_method(const TypeInfo &type, TypeId method_key) const noexcept;
        /// Same as the `TypeId` overload, looked up by name. When `type` (or a reflected
        /// ancestor) declares more than one method with `method_name` (an overload set), this
        /// returns whichever was declared first — use the `param_types` overload below to
        /// disambiguate.
        [[nodiscard]] const MethodInfo *find_method(const TypeInfo &type, std::string_view method_name) const noexcept;
        /// Finds the method named `method_name` whose parameter types match `param_types` exactly
        /// on `type` or its nearest reflected ancestor — the inheritance-aware, disambiguating
        /// lookup for an overload set. See `TypeInfo::find_method`'s matching overload.
        ///
        /// @return Returns a pointer to the requested method, or `nullptr` when unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MethodInfo *find_method(const TypeInfo &type, std::string_view method_name, std::span<const TypeId> param_types) const noexcept;
        /// Finds `event_key` on `type` or its nearest reflected ancestor. See `find_field`.
        ///
        /// @return Returns a pointer to the requested event, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EventInfo *find_event(const TypeInfo &type, TypeId event_key) const noexcept;
        /// Same as the `TypeId` overload, looked up by name.
        [[nodiscard]] const EventInfo *find_event(const TypeInfo &type, std::string_view event_name) const noexcept;

        /// Installs a mod override on `type_key`'s `method_key` method. `override_user_data` is
        /// written before the atomic publish of `override_fn` (release store), so a reader that
        /// observes the new `override_fn` via an acquire load is guaranteed to see the matching
        /// `user_data` too.
        ///
        /// @return Returns `true` when the type/method was found; `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool set_method_override(TypeId type_key, TypeId method_key, MethodInvokeFn override_fn, void *user_data) noexcept;
        /// Clears a previously installed mod override.
        ///
        /// @return Returns `true` when the type/method was found; `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear_method_override(TypeId type_key, TypeId method_key) noexcept;

        /// Adds a listener that fires just before `method_key` runs (see `MethodInfo::before_invoke`).
        /// Unlike `set_method_override`, any number of mods may add hooks without clobbering
        /// each other's.
        ///
        /// @return Returns an engaged `Subscription` on success; `std::nullopt` when the
        /// type/method was not found.
        [[nodiscard]] std::optional<Multicast::Subscription> add_method_before_hook(TypeId type_key, TypeId method_key, Multicast::ListenerFn hook, void *user_data) noexcept;
        /// Removes a hook added by `add_method_before_hook`.
        ///
        /// @return Returns `true` when found and removed; `false` otherwise.
        [[nodiscard]] bool remove_method_before_hook(TypeId type_key, TypeId method_key, Multicast::Subscription subscription) noexcept;
        /// Adds a listener that fires just after `method_key` returns (see `MethodInfo::after_invoke`).
        ///
        /// @return Returns an engaged `Subscription` on success; `std::nullopt` when the
        /// type/method was not found.
        [[nodiscard]] std::optional<Multicast::Subscription> add_method_after_hook(TypeId type_key, TypeId method_key, Multicast::ListenerFn hook, void *user_data) noexcept;
        /// Removes a hook added by `add_method_after_hook`.
        ///
        /// @return Returns `true` when found and removed; `false` otherwise.
        [[nodiscard]] bool remove_method_after_hook(TypeId type_key, TypeId method_key, Multicast::Subscription subscription) noexcept;

        /// Subscribes a listener to `event_key` on `type_key` (see `EventInfo`/`fire_event`).
        ///
        /// @return Returns an engaged `Subscription` on success; `std::nullopt` when the
        /// type/event was not found.
        [[nodiscard]] std::optional<Multicast::Subscription> subscribe_event(TypeId type_key, TypeId event_key, Multicast::ListenerFn listener, void *user_data) noexcept;
        /// Removes a listener added by `subscribe_event`.
        ///
        /// @return Returns `true` when found and removed; `false` otherwise.
        [[nodiscard]] bool unsubscribe_event(TypeId type_key, TypeId event_key, Multicast::Subscription subscription) noexcept;

        /// Reports whether `derived` is `base` itself, or inherits from it (directly or
        /// transitively) through `TypeInfo::base_type` — the Java-`Class.isAssignableFrom` analog
        /// (`base.isAssignableFrom(derived)` in Java's argument order; the same order here).
        /// Since reflected types are not required to be polymorphic and this system has no vtable
        /// to inspect, this only ever answers the static question "does `derived`'s declared
        /// inheritance chain include `base`" — there is no dynamic per-object runtime type to ask
        /// about without a real `TypeId` already in hand (unlike Java's `Class.isInstance(Object)`,
        /// which inspects a live object's actual runtime class).
        ///
        /// @return Returns `true` when `derived == base` or `base` appears somewhere in
        /// `derived`'s reflected base chain; `false` otherwise (including when either `TypeId` is
        /// not a registered type).
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_assignable_from(TypeId base, TypeId derived) const noexcept;

        /// Finds every currently-registered type carrying an attribute with `attribute_key` (see
        /// `Attribute`/`find_attribute`) — the reverse of `find_attribute`, which requires already
        /// having a specific `TypeInfo` in hand. Lets a mod discover, e.g., "every type tagged
        /// `SFT_ATTR_BOOL(\"modder_visible\", true)`" without needing to already know their names.
        /// A linear scan over every registered type — fine for tooling/discovery use (called
        /// rarely, not per-frame), not a hot path.
        ///
        /// @return Returns every registered type whose own `TypeInfo::attributes` contains a
        /// matching entry, in registration order.
        [[nodiscard]] std::vector<const TypeInfo *> find_types_with_attribute(TypeId attribute_key) const;
        /// Same as the `TypeId` overload, looked up by attribute name.
        [[nodiscard]] std::vector<const TypeInfo *> find_types_with_attribute(std::string_view attribute_name) const;

        /// Returns the number of registered types.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize size() const noexcept;

        /// Calls `visitor(const TypeInfo &)` once for every currently *registered* type (types
        /// removed by `unregister_type` are skipped), holding the registry's read lock for the
        /// duration. This is how a mod discovers what else is registered — including types other
        /// mods registered dynamically via `TypeInfoBuilder` — without needing to already know a
        /// name to look up.
        ///
        /// @note `visitor` must not re-enter the registry (register/find/subscribe/etc.); doing
        /// so while the read lock is held would deadlock against `register_type`'s write lock.
        template <class Visitor>
        void for_each_type(Visitor &&visitor) const {
            ZoneScopedN("TypeRegistry::for_each_type");
            std::shared_lock lock(mutex_);
            for (usize index = 0; index < infos_.size(); ++index) {
                if (!revoked_[index]) {
                    visitor(infos_[index]);
                }
            }
        }

        /// Registers a fully-built `EnumInfo`. Enums are stored separately from `TypeInfo` (see
        /// `EnumInfo`'s doc comment) but share this same registry — one well-known place to
        /// resolve any reflected name, whether it's a struct or an enum.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why
        /// the operation failed.
        [[nodiscard]] TypeRegistryExpected<TypeId> register_enum(EnumInfo info);

        /// Registers enum `T` on first use. Mirrors `try_register<T>()`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why
        /// the operation failed.
        template <class T>
        [[nodiscard]] TypeRegistryExpected<TypeId> try_register_enum() {
            ZoneScopedN("TypeRegistry::try_register_enum");
            using EnumT = std::remove_cv_t<T>;
            constexpr std::string_view name = Detail::enum_name<EnumT>();
            const TypeId key = TypeId::from_name(name);
            if (find_enum(key) != nullptr) {
                return key;
            }
            return register_enum(Detail::make_enum_info<EnumT>());
        }

        /// Returns enum `T`'s `EnumInfo`, registering it on first use. Mirrors `type<T>()`.
        ///
        /// @return Returns a reference to the enum's descriptor.
        template <class T>
        [[nodiscard]] const EnumInfo &enum_type() {
            ZoneScopedN("TypeRegistry::enum_type");
            using EnumT = std::remove_cv_t<T>;
            auto registered = try_register_enum<EnumT>();
            if (!registered) {
                Detail::contract_violation(
                    "Reflection enum resolution failed for '{}': {}",
                    Detail::enum_name<EnumT>(),
                    registered.error().message);
            }
            const EnumInfo *found = find_enum(*registered);
            if (found == nullptr) {
                Detail::contract_violation(
                    "Reflection enum '{}' vanished immediately after registration.",
                    Detail::enum_name<EnumT>());
            }
            return *found;
        }

        /// Finds the requested enum by `TypeId`.
        ///
        /// @return Returns a pointer to the requested enum, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EnumInfo *find_enum(TypeId key) const noexcept;
        /// Finds the requested enum by canonical name.
        ///
        /// @return Returns a pointer to the requested enum, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const EnumInfo *find_enum(const ustr &canonical_name) const noexcept;

      private:
        TypeRegistry() = default;
        ~TypeRegistry() = default;

        mutable std::shared_mutex mutex_;
        std::deque<TypeInfo> infos_;
        /// Parallel to `infos_`, index-for-index. `unregister_type` sets an entry rather than
        /// erasing from `infos_` (see `unregister_type`'s doc comment for why); this is what lets
        /// `for_each_type` still skip it in O(1) without hashing every entry against the maps.
        std::deque<bool> revoked_;
        std::unordered_map<TypeId, usize, TypeIdHash> ids_by_key_;
        std::unordered_map<UString, usize> ids_by_name_;

        std::deque<EnumInfo> enum_infos_;
        std::unordered_map<TypeId, usize, TypeIdHash> enum_ids_by_key_;
        std::unordered_map<UString, usize> enum_ids_by_name_;
    };


} // namespace SFT::Reflection
