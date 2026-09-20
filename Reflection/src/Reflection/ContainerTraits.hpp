#pragma once

#include <Foundation/Foundation.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// Standard-library container/wrapper detection traits, shared by every layer of the reflection
/// package that needs to recognize a shape rather than just a type.
///
/// These lived in `Macros.hpp` (where `Detail::build_field_info` first needed them) until
/// `TypeShape.hpp` and `StructuralTypeId.hpp` came to need them too. `StructuralTypeId.hpp` has to
/// be usable *from* `Macros.hpp` (so the runtime `Detail::erased_type_id` can compose a stable
/// identity for a container instead of falling back to `typeid`), which made the old arrangement
/// circular: `Macros.hpp` -> `StructuralTypeId.hpp` -> `TypeShape.hpp` -> `Macros.hpp`. Hoisting
/// the traits into this dependency-free leaf header breaks that cycle; nothing here depends on
/// anything else in the package.
namespace SFT::Reflection::Detail {

    /// Detects `std::vector<T, Alloc>` specifically (the only recognized sequence container — see
    /// `ContainerInfo`'s doc comment; `std::array<T, N>` is handled separately by `IsStdArray`
    /// below, since it needs a fixed-size accessor rather than a resizable one).
    template <class T>
    struct IsStdVector : std::false_type {};
    template <class T, class Alloc>
    struct IsStdVector<std::vector<T, Alloc>> : std::true_type {
        using Element = T;
    };

    /// Detects `std::array<T, N>`.
    template <class T>
    struct IsStdArray : std::false_type {};
    template <class T, usize N>
    struct IsStdArray<std::array<T, N>> : std::true_type {
        using Element = T;
        static constexpr usize Size = N;
    };

    /// Detects `std::unordered_map<K, V, Hash, Eq, Alloc>`.
    template <class T>
    struct IsStdUnorderedMap : std::false_type {};
    template <class K, class V, class Hash, class Eq, class Alloc>
    struct IsStdUnorderedMap<std::unordered_map<K, V, Hash, Eq, Alloc>> : std::true_type {
        using Key = K;
        using Value = V;
    };

    /// Detects `std::map<K, V, Compare, Alloc>` (ordered) — reuses the exact same runtime
    /// `MapInfo` shape as `std::unordered_map`: `MapInfo`'s operations (`for_each`/
    /// `insert_or_assign`/`find`/`erase`/`clear`) never depended on hashing vs. ordering in the
    /// first place, so the only thing that differs is which container type the generated lambdas
    /// close over (see `map_info_for`).
    template <class T>
    struct IsStdMap : std::false_type {};
    template <class K, class V, class Compare, class Alloc>
    struct IsStdMap<std::map<K, V, Compare, Alloc>> : std::true_type {
        using Key = K;
        using Value = V;
    };

    /// Detects `std::set<T, Compare, Alloc>`.
    template <class T>
    struct IsStdSet : std::false_type {};
    template <class T, class Compare, class Alloc>
    struct IsStdSet<std::set<T, Compare, Alloc>> : std::true_type {
        using Element = T;
    };

    /// Detects `std::unordered_set<T, Hash, Eq, Alloc>`.
    template <class T>
    struct IsStdUnorderedSet : std::false_type {};
    template <class T, class Hash, class Eq, class Alloc>
    struct IsStdUnorderedSet<std::unordered_set<T, Hash, Eq, Alloc>> : std::true_type {
        using Element = T;
    };

    /// Detects `std::optional<T>`.
    template <class T>
    struct IsStdOptional : std::false_type {};
    template <class T>
    struct IsStdOptional<std::optional<T>> : std::true_type {
        using Value = T;
    };

    /// Detects `std::unique_ptr<T, Deleter>` (the default deleter only — a custom deleter can't
    /// generically be reconstructed from a bare copy of `*T`, so it falls through to the opaque
    /// non-trivial-type path like any other unrecognized shape).
    template <class T>
    struct IsStdUniquePtr : std::false_type {};
    template <class T>
    struct IsStdUniquePtr<std::unique_ptr<T>> : std::true_type {
        using Value = T;
    };

    /// Detects `std::shared_ptr<T>`.
    template <class T>
    struct IsStdSharedPtr : std::false_type {};
    template <class T>
    struct IsStdSharedPtr<std::shared_ptr<T>> : std::true_type {
        using Value = T;
    };

} // namespace SFT::Reflection::Detail
