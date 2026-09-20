#pragma once

#include <Reflection/Contract.hpp>
#include <Reflection/EnumInfo.hpp>
#include <Reflection/Handle.hpp>
#include <Reflection/Macros.hpp>
#include <Reflection/RuntimeOverlay.hpp>
#include <Reflection/StaticReflection.hpp>
#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <deque>
#include <expected>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <utility>
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
            // Secondary bases (`SFT_REFLECT_TYPE_WITH_BASES`) need the same cascade, for the same
            // reason: `find_field_adjusted`/`find_method_adjusted`'s runtime walk reaches them
            // through `TypeInfo::secondary_bases` -> `find(TypeId)`, not through compile-time
            // knowledge of `TypeTraits<T>::SecondaryBaseTypes`.
            if constexpr (requires { typename TypeTraits<TypeT>::SecondaryBaseTypes; }) {
                using SecondaryBases = typename TypeTraits<TypeT>::SecondaryBaseTypes;
                const bool secondary_ok = [this]<class... Bases>(std::type_identity<std::tuple<Bases...>>) {
                    return (this->template try_register<Bases>().has_value() && ...);
                }(std::type_identity<SecondaryBases>{});
                if (!secondary_ok) {
                    return std::unexpected(TypeRegistryError{
                        .code = TypeRegistryErrorCode::InvalidDescriptor,
                        .message = UString{"a secondary base failed to register"},
                    });
                }
            }
            TypeInfo info = Detail::make_type_info<TypeT>();
            // The compile-time layer acting as authority the runtime layer is checked against,
            // rather than the other way around: `StaticTypeInfo<T>::field_count()` walks the
            // exact same `TypeTraits<T>::for_each_member` `Detail::make_type_info` just did, and
            // now that `SFT_ATTR_BOOL`/`_INT`/`_FLOAT`/`_STRING` (`Attribute.hpp`) expand to
            // captureless attribute *factories* rather than already-constructed `Attribute`
            // values, constructing them as unused arguments to a `consteval` visitor no longer
            // evaluates `Detail::make_attribute`'s non-`constexpr` `UString` construction — so
            // this is unconditionally safe for every reflected type, attributed fields included.
            // A divergence here means `Detail::make_type_info`/`Detail::static_field_count`
            // itself has a bug, not a legitimate reflection-usage error — hence terminating
            // rather than returning an error a caller could plausibly hit at runtime.
            if (info.fields.size() != StaticTypeInfo<TypeT>::field_count()) {
                Detail::contract_violation(
                    "Reflection type '{}': runtime TypeInfo::fields.size() ({}) disagrees with "
                    "StaticTypeInfo<T>::field_count() ({}) -- the compile-time and runtime "
                    "descriptor-building walks over TypeTraits<T>::for_each_member diverged.",
                    Detail::type_name<TypeT>(), info.fields.size(), StaticTypeInfo<TypeT>::field_count());
            }
            return register_type(std::move(info));
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

        /// Finds `field_key` on `type` or reachable through ANY of its reflected bases — the
        /// primary (`TypeInfo::base_type`) chain and every `secondary_bases` entry, recursively —
        /// returning both the `FieldInfo` and an `object` pointer already adjusted to point at the
        /// subobject that actually declares it.
        ///
        /// This exists because `find_field`/`copy_field_out` alone are NOT safe to use across a
        /// secondary base: `copy_field_out` applies `FieldInfo::offset` directly to whatever object
        /// pointer it's given, and that offset was computed against the *declaring* type
        /// (`Detail::member_offset<Base>`, not `<Derived>`) — correct only when the base subobject
        /// sits at the same address as the derived object, which `TypeInfo::base_type`'s doc
        /// comment establishes for the primary base but `secondary_bases`' doc comment explicitly
        /// does not. Passing `object` through unadjusted to a field found via a secondary base
        /// would silently read/write the wrong bytes. Use this instead of `find_field` whenever a
        /// real object pointer is involved and the type might have secondary bases; `find_field`
        /// remains correct and cheaper for primary-chain-only lookups (single inheritance, or pure
        /// declaration discovery with no object in hand).
        ///
        /// @return Returns the field and its correctly adjusted object pointer, or
        /// `{nullptr, nullptr}` when no reflected base declares `field_key`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::pair<const FieldInfo *, void *> find_field_adjusted(const TypeInfo &type, TypeId field_key, void *object) const noexcept;
        /// Same as the `TypeId` overload, looked up by name.
        [[nodiscard]] std::pair<const FieldInfo *, void *> find_field_adjusted(const TypeInfo &type, std::string_view field_name, void *object) const noexcept;

        /// Finds `method_key` on `type` or reachable through any reflected base (primary or
        /// secondary), returning the method and a correctly adjusted receiver pointer, ready to
        /// pass to `invoke_method`. See `find_field_adjusted`'s doc comment — the same "a secondary
        /// base's methods were compiled against a `Base *this`, not the derived object's address"
        /// hazard applies to invocation exactly as it does to field offsets.
        ///
        /// @return Returns the method and its correctly adjusted receiver pointer, or
        /// `{nullptr, nullptr}` when no reflected base declares `method_key`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::pair<const MethodInfo *, void *> find_method_adjusted(const TypeInfo &type, TypeId method_key, void *object) const noexcept;
        /// Same as the `TypeId` overload, looked up by name (first declared match — see
        /// `find_method`'s by-name overload for why this doesn't disambiguate overload sets).
        [[nodiscard]] std::pair<const MethodInfo *, void *> find_method_adjusted(const TypeInfo &type, std::string_view method_name, void *object) const noexcept;

        /// Returns a stable, generation-checked `TypeHandle` for `key`, or an invalid (default)
        /// `TypeHandle` when `key` is not currently registered. See `TypeHandle`'s doc comment for
        /// why a caller would want this instead of holding onto a `const TypeInfo *` directly.
        ///
        /// @return Returns the newly constructed handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] TypeHandle handle_for(TypeId key) const noexcept;
        /// Resolves `handle` back to its `TypeInfo`, or `nullptr` when the slot it names has been
        /// revoked (`unregister_type`) since `handle` was obtained — an O(1) index-plus-generation
        /// check, no hashing.
        ///
        /// @return Returns a pointer to the requested type, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const TypeInfo *resolve(TypeHandle handle) const noexcept;

        /// Returns a stable `FieldHandle` for the field named `field_key`, declared directly on
        /// (not inherited into) the type `owner` names — searches `TypeInfo::fields` and
        /// `TypeInfo::static_fields`, matching `TypeInfo::find_field`'s declared-only scope. An
        /// invalid `FieldHandle` when `owner` is stale or declares no such field.
        ///
        /// @return Returns the newly constructed handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] FieldHandle field_handle(TypeHandle owner, TypeId field_key) const noexcept;
        /// Resolves `handle` back to its `FieldInfo`. Re-resolves `handle.owner` first, so this
        /// returns `nullptr` once the owning type has been unregistered, even though the
        /// underlying `FieldInfo` storage is never freed.
        ///
        /// @return Returns a pointer to the requested field, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const FieldInfo *resolve(FieldHandle handle) const noexcept;

        /// Overrides `handle`'s effective name (see `effective_field_name`), without mutating the
        /// underlying `FieldInfo::name` any other caller still sees. Replaces any previously set
        /// override for the same handle.
        ///
        /// @return Returns `true` when `handle` resolves; `false` otherwise (nothing is stored).
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool set_field_name_override(FieldHandle handle, UString name) noexcept;
        /// Clears a name override installed by `set_field_name_override`, if any. Does not affect
        /// any attribute overlay on the same field — use `clear_field_overlay` to remove both.
        ///
        /// @return Returns `true` when `handle` resolved and an override was actually present
        /// (and is now cleared); `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear_field_name_override(FieldHandle handle) noexcept;
        /// Adds one mod-attached attribute to `handle`, additive to (never replacing) the field's
        /// own static `FieldInfo::attributes` — see `effective_field_attributes`.
        ///
        /// @return Returns `true` when `handle` resolves; `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool add_field_attribute_override(FieldHandle handle, Attribute attribute) noexcept;
        /// Removes every overlay (name override and attached attributes alike) for `handle`.
        ///
        /// @return Returns `true` when `handle` resolved and an overlay actually existed (and is
        /// now removed); `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear_field_overlay(FieldHandle handle) noexcept;

        /// Returns `handle`'s effective name: the mod-installed override if
        /// `set_field_name_override` has one, otherwise the field's own static
        /// `FieldInfo::name` — the "override if present, otherwise static" resolution the design
        /// doc calls for (`field name -> potentially RuntimeMutable`). An empty `UString` when
        /// `handle` does not resolve.
        ///
        /// @return Returns the current value.
        [[nodiscard]] UString effective_field_name(FieldHandle handle) const;
        /// Returns `handle`'s effective attribute set: the field's own static
        /// `FieldInfo::attributes`, followed by any mod-attached attributes from
        /// `add_field_attribute_override` — additive, not a replacement (a mod attribute with the
        /// same name as a static one shadows it only in the sense that `find_attribute` over the
        /// result returns whichever was declared first, matching that function's existing overload
        /// resolution semantics elsewhere in this package). Empty when `handle` does not resolve.
        ///
        /// @return Returns the current value.
        [[nodiscard]] std::vector<Attribute> effective_field_attributes(FieldHandle handle) const;

        /// Overrides `handle`'s effective name (see `effective_type_name`) — the type-level
        /// counterpart of `set_field_name_override`. Replaces any previously set override for the
        /// same handle.
        ///
        /// @return Returns `true` when `handle` resolves; `false` otherwise (nothing is stored).
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool set_type_name_override(TypeHandle handle, UString name) noexcept;
        /// Clears a name override installed by `set_type_name_override`, if any.
        ///
        /// @return Returns `true` when `handle` resolved and an override was actually present
        /// (and is now cleared); `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear_type_name_override(TypeHandle handle) noexcept;
        /// Adds one mod-attached attribute to `handle`, additive to `TypeInfo::attributes` — see
        /// `effective_type_attributes`.
        ///
        /// @return Returns `true` when `handle` resolves; `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool add_type_attribute_override(TypeHandle handle, Attribute attribute) noexcept;
        /// Removes every overlay (name override and attached attributes alike) for `handle`.
        ///
        /// @return Returns `true` when `handle` resolved and an overlay actually existed (and is
        /// now removed); `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear_type_overlay(TypeHandle handle) noexcept;
        /// Returns `handle`'s effective name: the mod-installed override if present, otherwise
        /// `TypeInfo::canonical_name` — the type-level counterpart of `effective_field_name`. An
        /// empty `UString` when `handle` does not resolve.
        ///
        /// @return Returns the current value.
        [[nodiscard]] UString effective_type_name(TypeHandle handle) const;
        /// Returns `handle`'s effective attribute set: `TypeInfo::attributes` followed by any
        /// mod-attached attributes — the type-level counterpart of `effective_field_attributes`.
        /// Empty when `handle` does not resolve.
        ///
        /// @return Returns the current value.
        [[nodiscard]] std::vector<Attribute> effective_type_attributes(TypeHandle handle) const;

        /// Returns a stable `MethodHandle` for the method keyed `method_key`, declared directly
        /// on `owner`. See `field_handle`.
        ///
        /// @return Returns the newly constructed handle.
        /// @note This function does not throw exceptions.
        [[nodiscard]] MethodHandle method_handle(TypeHandle owner, TypeId method_key) const noexcept;
        /// Resolves `handle` back to its `MethodInfo`. See the `FieldHandle` overload of `resolve`.
        ///
        /// @return Returns a pointer to the requested method, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const MethodInfo *resolve(MethodHandle handle) const noexcept;

        /// Overrides `handle`'s effective name (see `effective_method_name`) — the method-level
        /// counterpart of `set_field_name_override`. This changes how the method *presents*
        /// (e.g. to a script binding or an editor), not what it *does* — see
        /// `set_method_override`/`add_method_before_hook`/`_after_hook` for changing behavior.
        /// Replaces any previously set override for the same handle.
        ///
        /// @return Returns `true` when `handle` resolves; `false` otherwise (nothing is stored).
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool set_method_name_override(MethodHandle handle, UString name) noexcept;
        /// Clears a name override installed by `set_method_name_override`, if any.
        ///
        /// @return Returns `true` when `handle` resolved and an override was actually present
        /// (and is now cleared); `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear_method_name_override(MethodHandle handle) noexcept;
        /// Adds one mod-attached attribute to `handle`, additive to `MethodInfo::attributes` —
        /// see `effective_method_attributes`.
        ///
        /// @return Returns `true` when `handle` resolves; `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool add_method_attribute_override(MethodHandle handle, Attribute attribute) noexcept;
        /// Removes every overlay (name override and attached attributes alike) for `handle`.
        ///
        /// @return Returns `true` when `handle` resolved and an overlay actually existed (and is
        /// now removed); `false` otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool clear_method_overlay(MethodHandle handle) noexcept;
        /// Returns `handle`'s effective name: the mod-installed override if present, otherwise
        /// `MethodInfo::name` — the method-level counterpart of `effective_field_name`. An empty
        /// `UString` when `handle` does not resolve.
        ///
        /// @return Returns the current value.
        [[nodiscard]] UString effective_method_name(MethodHandle handle) const;
        /// Returns `handle`'s effective attribute set: `MethodInfo::attributes` followed by any
        /// mod-attached attributes — the method-level counterpart of `effective_field_attributes`.
        /// Empty when `handle` does not resolve.
        ///
        /// @return Returns the current value.
        [[nodiscard]] std::vector<Attribute> effective_method_attributes(MethodHandle handle) const;

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

        /// Unregisters an enum, so no future `find_enum`/`enum_type<T>()`/`try_register_enum<T>()`
        /// call can reach it — the enum-side counterpart to `unregister_type`, without which a mod
        /// that registered enums could never be fully unloaded (its `EnumInfo`s stayed reachable
        /// by name/key forever, and a reloaded version of the same mod hit a
        /// `CanonicalNameCollision` trying to re-register them).
        ///
        /// Like `unregister_type`, the `EnumInfo` itself is not physically freed — its slot stays
        /// in `enum_infos_` so that any `const EnumInfo *` already handed out does not dangle; only
        /// the name/key lookup entries are removed. The same quiescence requirement applies: stop
        /// dispatching against the enum before unloading the code its descriptor describes.
        ///
        /// @return Returns `true` when `key` was registered and is now unregistered; `false` when
        /// it was not found.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool unregister_enum(TypeId key) noexcept;
        /// Same as the `TypeId` overload, looked up by canonical name.
        [[nodiscard]] bool unregister_enum(const ustr &canonical_name) noexcept;

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

        /// The recursive body of `is_assignable_from`, walking both the primary `base_type` chain
        /// and every `secondary_bases` entry — assumes `mutex_` is already held (shared) by the
        /// caller; `std::shared_mutex` is not recursive, so this must never lock it itself.
        [[nodiscard]] bool is_assignable_from_locked(TypeId base, TypeId derived, usize depth) const noexcept;

        /// The depth-capped bodies behind `find_field_adjusted`/`find_method_adjusted` — same
        /// "fail closed on a misconfigured cyclic base chain rather than hang" discipline
        /// `max_base_chain_depth` already enforces for `find_field`/`find_method`/`find_event`.
        [[nodiscard]] std::pair<const FieldInfo *, void *> find_field_adjusted_impl(const TypeInfo &type, TypeId field_key, void *object, usize depth) const noexcept;
        [[nodiscard]] std::pair<const MethodInfo *, void *> find_method_adjusted_impl(const TypeInfo &type, TypeId method_key, void *object, usize depth) const noexcept;
        [[nodiscard]] std::pair<const MethodInfo *, void *> find_method_adjusted_by_name(const TypeInfo &type, std::string_view method_name, void *object, usize depth) const noexcept;

        /// Identifies a field for `field_overlays_` without needing `FieldHandle::owner`'s
        /// generation — a stale handle's `owner_index`/`field_index`/`is_static` still name the
        /// same slot they always did (indices are never reused), so the overlay itself doesn't
        /// need to move or vanish just because the handle checking against it goes stale; the
        /// public API's `resolve(handle.owner)` pass is what actually enforces staleness at the
        /// point of use.
        struct FieldOverlayKey {
            u32 owner_index = 0;
            u32 field_index = 0;
            bool is_static = false;

            friend constexpr bool operator==(const FieldOverlayKey &, const FieldOverlayKey &) noexcept = default;
        };
        struct FieldOverlayKeyHash {
            [[nodiscard]] usize operator()(FieldOverlayKey key) const noexcept {
                usize seed = std::hash<u32>{}(key.owner_index);
                seed ^= std::hash<u32>{}(key.field_index) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                seed ^= std::hash<bool>{}(key.is_static) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                return seed;
            }
        };

        /// Identifies a method for `method_overlays_` — same "index-based, generation-independent"
        /// rationale as `FieldOverlayKey`.
        struct MethodOverlayKey {
            u32 owner_index = 0;
            u32 method_index = 0;
            bool is_static = false;

            friend constexpr bool operator==(const MethodOverlayKey &, const MethodOverlayKey &) noexcept = default;
        };
        struct MethodOverlayKeyHash {
            [[nodiscard]] usize operator()(MethodOverlayKey key) const noexcept {
                usize seed = std::hash<u32>{}(key.owner_index);
                seed ^= std::hash<u32>{}(key.method_index) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                seed ^= std::hash<bool>{}(key.is_static) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
                return seed;
            }
        };

        mutable std::shared_mutex mutex_;
        std::deque<TypeInfo> infos_;
        /// Parallel to `infos_`, index-for-index. `unregister_type` sets an entry rather than
        /// erasing from `infos_` (see `unregister_type`'s doc comment for why); this is what lets
        /// `for_each_type` still skip it in O(1) without hashing every entry against the maps.
        std::deque<bool> revoked_;
        /// Parallel to `infos_`/`revoked_`. Starts at `1` for a freshly registered slot and is
        /// bumped by `unregister_type` — since slot indices are never reused (see `revoked_`'s
        /// doc comment), this exists purely to let a `TypeHandle` obtained *before* an
        /// `unregister_type` call detect that revocation (`resolve` compares generations) without
        /// needing to also re-check `revoked_` separately; a future in-place descriptor
        /// replacement (hot reload) would also bump this, which `revoked_` alone couldn't express
        /// (a replaced-but-still-registered slot is not "revoked").
        std::deque<u32> generations_;
        std::unordered_map<TypeId, usize, TypeIdHash> ids_by_key_;
        std::unordered_map<UString, usize> ids_by_name_;

        std::deque<EnumInfo> enum_infos_;
        std::unordered_map<TypeId, usize, TypeIdHash> enum_ids_by_key_;
        std::unordered_map<UString, usize> enum_ids_by_name_;

        /// Sparse by design (see `RuntimeOverlay.hpp`'s doc comment): a field nobody has ever
        /// overlaid has no entry here at all, not an entry holding empty/default state.
        std::unordered_map<FieldOverlayKey, FieldOverlay, FieldOverlayKeyHash> field_overlays_;
        /// Keyed by `TypeHandle::index` directly — a `TypeHandle` has no field/method sub-index
        /// to fold into a compound key the way `FieldOverlayKey`/`MethodOverlayKey` need to.
        /// Sparse by design, same rationale as `field_overlays_`.
        std::unordered_map<u32, TypeOverlay> type_overlays_;
        /// Sparse by design, same rationale as `field_overlays_`.
        std::unordered_map<MethodOverlayKey, MethodOverlay, MethodOverlayKeyHash> method_overlays_;
    };


} // namespace SFT::Reflection
