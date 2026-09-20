#pragma once

#include <Foundation/Foundation.hpp>

namespace SFT::Reflection {


    /// A stable, generation-checked runtime identity for a registered `TypeInfo`, meant to
    /// replace a bare `const TypeInfo *` for callers that need to hold onto a reference across
    /// time (a script binding, an editor selection, a saved mod-config entry) rather than for the
    /// duration of a single call.
    ///
    /// Unlike a raw pointer, resolving a `TypeHandle` (`TypeRegistry::resolve`) safely reports
    /// "no longer valid" once `TypeRegistry::unregister_type` has revoked the slot it names, even
    /// though — per `TypeRegistry::unregister_type`'s doc comment — the underlying `TypeInfo`
    /// storage is never physically freed (a raw pointer obtained before revocation would keep
    /// "working" in the sense of not crashing, but silently point at dead reflection state a mod
    /// unload was supposed to retire). `index` is stable for the process's remaining lifetime
    /// (registry slots are never reused), so a `TypeHandle` never needs to store a name/`TypeId`
    /// to be resolvable — `TypeRegistry::resolve` is an O(1) index-plus-generation check, not a
    /// hash lookup.
    struct TypeHandle {
        static constexpr u32 invalid_index = ~0u;

        u32 index = invalid_index;
        u32 generation = 0;

        /// Converts the `TypeHandle` to `bool`.
        ///
        /// @return Returns `true` when this handle names a slot at all (says nothing about
        /// whether that slot is still live — use `TypeRegistry::resolve` for that).
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return index != invalid_index;
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const TypeHandle &, const TypeHandle &) noexcept = default;
    };

    /// A stable identity for one field declared directly on a specific registered type — relative
    /// to `owner`, never implicitly walking into a base type the way
    /// `TypeRegistry::find_field`'s inheritance-aware lookup does (resolve `owner`'s base's own
    /// `FieldHandle` separately if that's what's needed). Resolving a `FieldHandle`
    /// (`TypeRegistry::resolve`) re-resolves `owner` first, so a field on an unregistered type
    /// becomes unresolvable exactly when its owning type does, even though — like `TypeInfo`
    /// itself — the `FieldInfo` storage underneath is never physically freed or reallocated after
    /// registration (`index` stays valid for as long as `owner`'s generation matches).
    struct FieldHandle {
        TypeHandle owner{};
        u32 index = TypeHandle::invalid_index;
        /// Selects `TypeInfo::fields` (`false`) vs. `TypeInfo::static_fields` (`true`) — mirrors
        /// `FieldFlags::Static`'s existing role of picking which vector/accessor a `FieldInfo`
        /// belongs to.
        bool is_static = false;

        /// Converts the `FieldHandle` to `bool`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return static_cast<bool>(owner) && index != TypeHandle::invalid_index;
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const FieldHandle &, const FieldHandle &) noexcept = default;
    };

    /// A stable identity for one method declared directly on a specific registered type. See
    /// `FieldHandle`'s doc comment — everything there applies here, substituting
    /// `TypeInfo::methods`/`static_methods` for `fields`/`static_fields`.
    struct MethodHandle {
        TypeHandle owner{};
        u32 index = TypeHandle::invalid_index;
        /// Selects `TypeInfo::methods` (`false`) vs. `TypeInfo::static_methods` (`true`) — mirrors
        /// `MethodInfo::is_static`.
        bool is_static = false;

        /// Converts the `MethodHandle` to `bool`.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return static_cast<bool>(owner) && index != TypeHandle::invalid_index;
        }

        /// Compares the operands for equality.
        ///
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(const MethodHandle &, const MethodHandle &) noexcept = default;
    };


} // namespace SFT::Reflection
