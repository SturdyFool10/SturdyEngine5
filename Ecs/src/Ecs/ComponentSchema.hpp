#pragma once

#include <Foundation/Foundation.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Ecs {

    /// The portable scalar kinds a component field can have. These are what two languages must agree on to share one component's
    /// memory; a C++ `float`, a Rust `f32` and a C# `float` are all `F32`. Values are fixed forever (they are hashed into
    /// layout fingerprints and appear in the C ABI): never reorder or reuse one.
    enum class FieldKind : u32 {
        Bool = 0, ///< One byte, 0 or 1.
        I8 = 1,
        I16 = 2,
        I32 = 3,
        I64 = 4,
        U8 = 5,
        U16 = 6,
        U32 = 7,
        U64 = 8,
        F32 = 9,
        F64 = 10,
    };

    /// Size in bytes of one value of `kind`; 0 for a value outside the enumeration.
    [[nodiscard]] constexpr u32 field_kind_size(FieldKind kind) noexcept {
        switch (kind) {
            case FieldKind::Bool:
            case FieldKind::I8:
            case FieldKind::U8: return 1;
            case FieldKind::I16:
            case FieldKind::U16: return 2;
            case FieldKind::I32:
            case FieldKind::U32:
            case FieldKind::F32: return 4;
            case FieldKind::I64:
            case FieldKind::U64:
            case FieldKind::F64: return 8;
        }
        return 0;
    }

    /// One field of a component: `count` consecutive values of `kind` starting `offset` bytes into the component (a C array
    /// `float position[3]` is one field with `count == 3`).
    struct ComponentField {
        /// For diagnostics and tooling only. It is not part of the layout fingerprint, because naming conventions differ per
        /// language (`max_health`, `maxHealth`, `MaxHealth`) while the memory is the same.
        std::string name;
        FieldKind kind = FieldKind::U8;
        u32 offset = 0;
        u32 count = 1;
    };

    /// The field layout of a component, in any order; the fingerprint sorts by offset.
    struct ComponentSchema {
        std::vector<ComponentField> fields;
    };

    /// A 128-bit fingerprint of a component's memory layout: its size, alignment and every field's kind, offset and count.
    /// All zero means "no shape was supplied".
    struct LayoutFingerprint {
        u64 high = 0;
        u64 low = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return high != 0 || low != 0; }
        friend constexpr bool operator==(LayoutFingerprint, LayoutFingerprint) noexcept = default;
    };

    /// Checks that `fields` describe a possible layout inside a component of `size` bytes: known kinds, nonzero counts, every
    /// field within the component, and no two fields overlapping.
    ///
    /// @return The reason it is invalid, or an empty optional when it is valid.
    [[nodiscard]] std::optional<std::string> validate_component_fields(usize size, std::span<const ComponentField> fields);

    /// Fingerprints a validated layout. Independent of field order and field names; a different size, alignment, kind, offset
    /// or count gives a different fingerprint (and never the all-zero "none" value).
    [[nodiscard]] LayoutFingerprint fingerprint_component_layout(usize size, usize align, std::span<const ComponentField> fields);

    /// What differs between two layouts of the same component, for an error message: the first mismatched size, alignment or
    /// field (by sorted offset), quoting both sides. Empty when they are identical.
    [[nodiscard]] std::string describe_layout_difference(usize size_a, usize align_a, std::span<const ComponentField> fields_a, usize size_b,
                                                         usize align_b, std::span<const ComponentField> fields_b);

} // namespace SFT::Ecs
