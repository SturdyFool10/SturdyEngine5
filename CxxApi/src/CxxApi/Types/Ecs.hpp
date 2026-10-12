#pragma once

// Plain-data types for components, systems and their declared access. bindgen-safe: <cstddef>/<cstdint> and other Types/ headers
// only (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// The portable scalar kinds a component field can have: what two languages must agree on to share a component's memory
    /// (a C++ `float`, a Rust `f32` and a C# `float` are all `F32`). Mirrors `Ecs::FieldKind`; values never change.
    enum class FieldKind : std::uint32_t {
        Bool = 0,
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

    /// `count` consecutive values of `kind`, `offset` bytes into a component (`float position[3]` is one field with `count == 3`).
    /// Names travel separately (`ecs_register_component`'s `field_names`) because they are diagnostics, not layout.
    struct ComponentFieldDesc {
        FieldKind kind = FieldKind::U8;
        std::uint32_t offset = 0;
        std::uint32_t count = 1;
    };

    /// How a system touches a component. Two systems that only read a component can run at the same time; a writer excludes
    /// everything else touching it.
    enum class AccessMode : std::uint32_t {
        Read = 0,
        Write = 1,
    };

    /// One component a system iterates, and how.
    struct ComponentAccess {
        std::uint32_t component = 0;
        AccessMode mode = AccessMode::Read;
    };

    /// Identifies a resource or event channel; derived from its canonical name, so the same name gives the same id in every
    /// language and every run.
    struct ResourceId {
        std::uint64_t high = 0;
        std::uint64_t low = 0;
    };

    /// One resource or event channel a system touches, and how. The scheduler uses it exactly like component access: two systems
    /// that write the same resource never overlap, and a reader never overlaps a writer.
    struct ResourceAccess {
        ResourceId resource{};
        AccessMode mode = AccessMode::Read;
    };

    /// A component's layout fingerprint (size, alignment and each field's kind, offset and count; names are ignored). Both words
    /// zero means its registrant gave no field layout.
    struct LayoutFingerprint {
        std::uint64_t high = 0;
        std::uint64_t low = 0;
    };

} // namespace SFT::CxxApi
