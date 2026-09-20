#pragma once

#include <Reflection/Attribute.hpp>

#include <Foundation/Foundation.hpp>

#include <optional>
#include <vector>

namespace SFT::Reflection {


    /// Mod-mutable state layered over one field's otherwise-authoritative `FieldInfo`, keyed by
    /// `FieldHandle` in `TypeRegistry`. This is the "RuntimeMutable"/"ModOverrideable" half of the
    /// design doc's classification (`field name -> potentially RuntimeMutable`,
    /// `attributes -> RuntimeMutable`): a mod can rename how a field presents to tooling/scripting
    /// (`Player.health` shown as `Player.HP`) or attach its own tooling metadata, without ever
    /// mutating the declared `FieldInfo` every other caller (including other mods) still sees as
    /// ground truth. `TypeRegistry::effective_field_name`/`effective_field_attributes` are the
    /// only places that resolve "override if present, otherwise static" — `FieldInfo::name`/
    /// `attributes` themselves stay exactly as `Detail::build_field_info` built them.
    ///
    /// Deliberately not embedded in `FieldInfo` itself (unlike, say, `MethodInfo::override_fn`,
    /// which every call site must check): overlay lookups are keyed by handle in a
    /// `TypeRegistry`-owned side table (see `TypeRegistry::field_overlays_`), so a `FieldInfo`
    /// nobody has ever overlaid costs nothing beyond its own existing storage — no
    /// unconditionally-present overlay slot on every field the way `MethodInfo` used to carry an
    /// eagerly-allocated `Multicast` for every method (see `Multicast.hpp`'s lazy redesign).
    struct FieldOverlay {
        /// Present when a mod has overridden this field's effective name; absent otherwise.
        std::optional<UString> name;
        /// Additive mod-attached attributes, layered on top of (never replacing) the field's own
        /// static `attributes` — see `TypeRegistry::effective_field_attributes`.
        std::vector<Attribute> extra_attributes;

        /// Reports whether this overlay currently holds no actual override (a "should this
        /// side-table entry be erased" check, not a general emptiness query).
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_empty() const noexcept {
            return !name.has_value() && extra_attributes.empty();
        }
    };

    /// The type-level counterpart of `FieldOverlay` — lets a mod rename a whole registered
    /// `TypeInfo` (as it presents to tooling/scripting/other mods, e.g. `Player` shown as
    /// `PlayerCharacter`) or attach type-level tooling metadata, additive over
    /// `TypeInfo::canonical_name`/`attributes` without mutating either. Keyed by `TypeHandle` in
    /// `TypeRegistry::type_overlays_`; same sparse-by-default rationale as `FieldOverlay`.
    struct TypeOverlay {
        std::optional<UString> name;
        std::vector<Attribute> extra_attributes;

        /// Reports whether this overlay currently holds no actual override.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_empty() const noexcept {
            return !name.has_value() && extra_attributes.empty();
        }
    };

    /// The method-level counterpart of `FieldOverlay` — lets a mod rename a declared `MethodInfo`
    /// or attach method-level tooling metadata, additive over `MethodInfo::name`/`attributes`
    /// without mutating either. Deliberately separate from `MethodInfo::override_fn`/`Multicast`
    /// (`before_invoke`/`after_invoke`): those replace or observe *behavior*; this only changes
    /// how the method *presents* (its name and metadata), the same distinction `FieldOverlay`
    /// already draws for fields versus `TypeRegistry::set_method_override`. Keyed by
    /// `MethodHandle` in `TypeRegistry::method_overlays_`.
    struct MethodOverlay {
        std::optional<UString> name;
        std::vector<Attribute> extra_attributes;

        /// Reports whether this overlay currently holds no actual override.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_empty() const noexcept {
            return !name.has_value() && extra_attributes.empty();
        }
    };


} // namespace SFT::Reflection
