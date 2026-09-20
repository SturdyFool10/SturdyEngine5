#pragma once

#include <Reflection/StaticTypeId.hpp>
#include <Reflection/TypeShape.hpp>

#include <Foundation/Foundation.hpp>

#include <string>
#include <type_traits>

/// Extends `StaticTypeId.hpp`'s canonical identity from "types with a name of their own" to
/// *composed* types — `std::vector<game.Player>`, `std::optional<i32>`,
/// `std::unordered_map<i32, game.Item>` — by building their names structurally out of their
/// element/key/value types' own canonical names.
///
/// This closes the single biggest coverage gap in the compile-time layer. `type_id<T>()` is
/// deliberately a closed set (a reflected type, a reflected enum, or an entry in
/// `Detail::FundamentalTypeName`) so that identity can never silently come from
/// `typeid(T).name()`. But that also meant `StaticTypeInfo<T>::fields()` — which needs a
/// `TypeRef` per field — hard-failed for *any* struct with a `std::vector`/`std::optional`/
/// smart-pointer field, i.e. most real game types, and `schema_hash_of<T>()` inherited the same
/// limitation. Composing the name from parts keeps the "never RTTI, deterministic across
/// compilers and builds" guarantee completely intact (every piece is either a reflected name, an
/// enum name, or a fixed fundamental spelling) while making the static layer usable on types
/// people actually write.
///
/// A type that already has a stable name composes to exactly that name, so
/// `structural_type_id<T>() == type_id<T>()` for every `T` the latter accepts — this is an
/// extension of the existing identity scheme, not a second, competing one.
namespace SFT::Reflection {

    namespace Detail {

        template <class T>
        [[nodiscard]] consteval bool has_structural_type_id() noexcept;

        /// Appends `usize` as decimal digits, for `std::array<T, N>`'s `N`. `std::to_string` is not
        /// usable during constant evaluation, so this builds the digits by hand.
        constexpr void append_structural_extent(std::string &out, usize value) {
            if (value == 0) {
                out.push_back('0');
                return;
            }
            char digits[20]{};
            usize count = 0;
            while (value != 0) {
                digits[count++] = static_cast<char>('0' + static_cast<char>(value % 10));
                value /= 10;
            }
            while (count != 0) {
                out.push_back(digits[--count]);
            }
        }

        /// Reports whether `T` is a shape this file knows how to compose a name for *and* whose
        /// every constituent type is itself nameable, recursively.
        ///
        /// Written as a total `consteval` predicate that always returns a value — it never calls
        /// `type_id`/`stable_type_name` on a type that lacks one — specifically so it is safe to
        /// use as a `requires (has_structural_type_id<T>())` constraint. That distinction matters:
        /// a *simple-requirement* (`requires { structural_type_id<T>(); }`) would only check that
        /// the call expression matches the function's declared signature, not that its body could
        /// actually be evaluated, and would therefore answer `true` for unsupported types and then
        /// hard-error at the point of use. Evaluating a total predicate has no such trap.
        template <class T>
        [[nodiscard]] consteval bool has_structural_type_id_impl() noexcept {
            using Bare = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
            if constexpr (HasStableTypeId<Bare>) {
                return true;
            } else if constexpr (IsStdVector<Bare>::value) {
                return has_structural_type_id<typename IsStdVector<Bare>::Element>();
            } else if constexpr (IsStdArray<Bare>::value) {
                return has_structural_type_id<typename IsStdArray<Bare>::Element>();
            } else if constexpr (IsStdOptional<Bare>::value) {
                return has_structural_type_id<typename IsStdOptional<Bare>::Value>();
            } else if constexpr (IsStdUniquePtr<Bare>::value) {
                return has_structural_type_id<typename IsStdUniquePtr<Bare>::Value>();
            } else if constexpr (IsStdSharedPtr<Bare>::value) {
                return has_structural_type_id<typename IsStdSharedPtr<Bare>::Value>();
            } else if constexpr (IsStdSet<Bare>::value) {
                return has_structural_type_id<typename IsStdSet<Bare>::Element>();
            } else if constexpr (IsStdUnorderedSet<Bare>::value) {
                return has_structural_type_id<typename IsStdUnorderedSet<Bare>::Element>();
            } else if constexpr (IsStdMap<Bare>::value) {
                return has_structural_type_id<typename IsStdMap<Bare>::Key>() &&
                       has_structural_type_id<typename IsStdMap<Bare>::Value>();
            } else if constexpr (IsStdUnorderedMap<Bare>::value) {
                return has_structural_type_id<typename IsStdUnorderedMap<Bare>::Key>() &&
                       has_structural_type_id<typename IsStdUnorderedMap<Bare>::Value>();
            } else {
                return false;
            }
        }

        template <class T>
        [[nodiscard]] consteval bool has_structural_type_id() noexcept {
            return has_structural_type_id_impl<T>();
        }

        /// Appends `T`'s composed canonical name to `out`. Container spellings are the ordinary
        /// C++ ones (`std::vector<...>`) so a name read out of a schema dump or a mod manifest is
        /// recognizable to a human; the element names nested inside are whatever
        /// `stable_type_name` gives (a reflected type's own dotted canonical name, an enum's, or a
        /// fundamental spelling), never a compiler-specific mangling.
        template <class T>
            requires(has_structural_type_id<T>())
        constexpr void append_structural_type_name(std::string &out) {
            using Bare = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
            if constexpr (HasStableTypeId<Bare>) {
                out.append(stable_type_name<Bare>());
            } else if constexpr (IsStdVector<Bare>::value) {
                out.append("std::vector<");
                append_structural_type_name<typename IsStdVector<Bare>::Element>(out);
                out.push_back('>');
            } else if constexpr (IsStdArray<Bare>::value) {
                out.append("std::array<");
                append_structural_type_name<typename IsStdArray<Bare>::Element>(out);
                out.push_back(',');
                append_structural_extent(out, IsStdArray<Bare>::Size);
                out.push_back('>');
            } else if constexpr (IsStdOptional<Bare>::value) {
                out.append("std::optional<");
                append_structural_type_name<typename IsStdOptional<Bare>::Value>(out);
                out.push_back('>');
            } else if constexpr (IsStdUniquePtr<Bare>::value) {
                out.append("std::unique_ptr<");
                append_structural_type_name<typename IsStdUniquePtr<Bare>::Value>(out);
                out.push_back('>');
            } else if constexpr (IsStdSharedPtr<Bare>::value) {
                out.append("std::shared_ptr<");
                append_structural_type_name<typename IsStdSharedPtr<Bare>::Value>(out);
                out.push_back('>');
            } else if constexpr (IsStdSet<Bare>::value) {
                out.append("std::set<");
                append_structural_type_name<typename IsStdSet<Bare>::Element>(out);
                out.push_back('>');
            } else if constexpr (IsStdUnorderedSet<Bare>::value) {
                out.append("std::unordered_set<");
                append_structural_type_name<typename IsStdUnorderedSet<Bare>::Element>(out);
                out.push_back('>');
            } else if constexpr (IsStdMap<Bare>::value) {
                out.append("std::map<");
                append_structural_type_name<typename IsStdMap<Bare>::Key>(out);
                out.push_back(',');
                append_structural_type_name<typename IsStdMap<Bare>::Value>(out);
                out.push_back('>');
            } else {
                out.append("std::unordered_map<");
                append_structural_type_name<typename IsStdUnorderedMap<Bare>::Key>(out);
                out.push_back(',');
                append_structural_type_name<typename IsStdUnorderedMap<Bare>::Value>(out);
                out.push_back('>');
            }
        }

    } // namespace Detail

    /// Reports whether `structural_type_id<T>()`/`structural_type_ref<T>()` accept `T` — `true`
    /// for anything `type_id<T>()` already accepts, plus recognized container/wrapper shapes whose
    /// constituent types are themselves nameable.
    template <class T>
    concept StructurallyIdentifiable = Detail::has_structural_type_id<T>();

    /// `T`'s composed canonical identity. Equal to `type_id<T>()` for every `T` that has a name of
    /// its own; for a container/wrapper shape, the identity of its composed name (see
    /// `Detail::append_structural_type_name`).
    ///
    /// @return Returns the newly constructed id.
    template <class T>
        requires StructurallyIdentifiable<T>
    [[nodiscard]] consteval TypeId structural_type_id() noexcept {
        std::string name;
        Detail::append_structural_type_name<T>(name);
        return TypeId::from_name(name);
    }

    /// The `TypeRef`-producing equivalent of `structural_type_id<T>()` — same qualifier handling
    /// as `type_ref<T>()` (`StaticTypeId.hpp`), widened to composed shapes.
    ///
    /// @return Returns the newly constructed reference.
    template <class T>
        requires StructurallyIdentifiable<T>
    [[nodiscard]] consteval TypeRef structural_type_ref() noexcept {
        using Bare = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
        return TypeRef{.base = structural_type_id<Bare>(), .qualifiers = Detail::qualifiers_of<T>()};
    }

} // namespace SFT::Reflection
