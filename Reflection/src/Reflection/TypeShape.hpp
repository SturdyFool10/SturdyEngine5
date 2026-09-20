#pragma once

#include <Reflection/ContainerTraits.hpp>
#include <Reflection/TypeInfo.hpp>

#include <Foundation/Foundation.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace SFT::Reflection {

    /// A coarse structural classification for an arbitrary C++ type, independent of whether it
    /// happens to be reflected via `SFT_REFLECT_TYPE`. This exists because a growing number of
    /// call sites (serializers, inspectors, the mod-facing generic-field walker the audit calls
    /// out) had started re-deriving "is this a vector/map/set/optional" via their own ad-hoc
    /// `IsStdVector`-style checks scattered across the codebase, one per caller, each one a
    /// slightly different subset of what `ContainerTraits.hpp`'s `Detail::IsStd*` traits already detect for
    /// `build_field_info`. `shape_of<T>()` is the single funnel: one classification, computed the
    /// same way everywhere, that a caller can `switch` on instead of writing its own chain of
    /// `if constexpr (IsStdVector<T>::value) ... else if constexpr (IsStdMap<T>::value) ...`.
    ///
    /// `Struct` deliberately means "reflected", not "class/struct in the C++ grammar sense" — an
    /// unreflected aggregate has no `TypeTraits<T>::for_each_member` to walk, so from this
    /// package's point of view it is indistinguishable from any other `Unknown` opaque blob.
    enum class TypeShape {
        Primitive,
        Enum,
        Struct,
        Array,
        Sequence,
        Associative,
        Set,
        Optional,
        Pointer,
        Reference,
        Tuple,
        Variant,
        String,
        SmartPointer,
        Function,
        Unknown,
    };

    namespace Detail {

        /// Detects `std::tuple<Ts...>` and `std::pair<A, B>` — not covered by any `IsStd*` trait
        /// in `ContainerTraits.hpp`, since neither one has ever needed `FieldInfo` reflection support
        /// there. Kept separate from `TemplateArgumentsOf` (below) since `shape_of` only needs the
        /// yes/no answer, not the argument types.
        template <class T>
        struct IsStdTuple : std::false_type {};
        template <class... Ts>
        struct IsStdTuple<std::tuple<Ts...>> : std::true_type {};
        template <class A, class B>
        struct IsStdTuple<std::pair<A, B>> : std::true_type {};

        /// Detects `std::variant<Ts...>`.
        template <class T>
        struct IsStdVariant : std::false_type {};
        template <class... Ts>
        struct IsStdVariant<std::variant<Ts...>> : std::true_type {};

        /// Detects `std::function<Sig>`. Raw function pointers/references are handled directly in
        /// `shape_of` via `std::is_function_v`/`std::is_pointer_v<std::remove_pointer_t<...>>`
        /// rather than a trait here, since they aren't class templates to specialize against.
        template <class T>
        struct IsStdFunction : std::false_type {};
        template <class Sig>
        struct IsStdFunction<std::function<Sig>> : std::true_type {};

        /// True for `T` being a function type, a pointer to one, or `std::function<...>` — the
        /// three shapes callable-typed fields/members tend to actually appear as. `std::is_pointer_v`
        /// alone would also be true here (a function pointer is a pointer), which is exactly why
        /// this check must run before the generic raw-pointer check in `shape_of`.
        template <class T>
        concept CallableTypeShape =
            std::is_function_v<T> ||
            (std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>) ||
            IsStdFunction<T>::value;

    } // namespace Detail

    /// Classifies `T` into a `TypeShape`. `T` is taken exactly as the caller names it (not
    /// pre-stripped) specifically so reference-ness can be observed before anything else strips
    /// it away: `shape_of<Player &>()` must answer `Reference`, not silently degrade to whatever
    /// `Player` itself classifies as, since "this is a reference" is information a generic walker
    /// (deciding whether it can bind a new value in place, say) needs to see first. Pointers are
    /// resolved the same way, one level down, on the reference-stripped type — `int * &` is a
    /// `Reference` (to a pointer), while plain `int *` is a `Pointer`.
    ///
    /// Every other case classifies `std::remove_cvref_t<T>` — a `const std::vector<int>&` is a
    /// `Sequence` in exactly the same way a plain `std::vector<int>` is, `const`/`volatile` alone
    /// carry no structural information this enum is trying to capture.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    template <class T>
    [[nodiscard]] consteval TypeShape shape_of() noexcept {
        if constexpr (std::is_lvalue_reference_v<T> || std::is_rvalue_reference_v<T>) {
            return TypeShape::Reference;
        } else if constexpr (Detail::CallableTypeShape<std::remove_cvref_t<T>>) {
            // Must run ahead of the raw is_pointer_v check below: a function pointer is also a
            // pointer, but "callable" is the more useful classification for it.
            return TypeShape::Function;
        } else if constexpr (std::is_pointer_v<T>) {
            return TypeShape::Pointer;
        } else {
            using Bare = std::remove_cvref_t<T>;
            if constexpr (std::is_enum_v<Bare>) {
                return TypeShape::Enum;
            } else if constexpr (std::is_arithmetic_v<Bare>) {
                return TypeShape::Primitive;
            } else if constexpr (std::is_same_v<Bare, std::string> ||
                                  std::is_same_v<Bare, std::string_view> ||
                                  std::is_same_v<Bare, SFT::Foundation::UString>) {
                return TypeShape::String;
            } else if constexpr (Detail::IsStdArray<Bare>::value) {
                return TypeShape::Array;
            } else if constexpr (Detail::IsStdVector<Bare>::value) {
                return TypeShape::Sequence;
            } else if constexpr (Detail::IsStdMap<Bare>::value || Detail::IsStdUnorderedMap<Bare>::value) {
                return TypeShape::Associative;
            } else if constexpr (Detail::IsStdSet<Bare>::value || Detail::IsStdUnorderedSet<Bare>::value) {
                return TypeShape::Set;
            } else if constexpr (Detail::IsStdOptional<Bare>::value) {
                return TypeShape::Optional;
            } else if constexpr (Detail::IsStdUniquePtr<Bare>::value || Detail::IsStdSharedPtr<Bare>::value) {
                return TypeShape::SmartPointer;
            } else if constexpr (Detail::IsStdTuple<Bare>::value) {
                return TypeShape::Tuple;
            } else if constexpr (Detail::IsStdVariant<Bare>::value) {
                return TypeShape::Variant;
            } else if constexpr (!TypeTraits<Bare>::name.empty()) {
                return TypeShape::Struct;
            } else {
                return TypeShape::Unknown;
            }
        }
    }

    /// Exposes a container/wrapper type's template argument(s) as member type aliases (and, for
    /// `std::array`, its fixed `Size`), so a caller that already knows a value's `TypeShape` can
    /// recover what it's a shape *of* — the audit's own motivating example is a generic walker
    /// seeing `Sequence` for a `vector<Player>` field and wanting `Player`'s own `TypeId` without
    /// re-deriving it from scratch.
    ///
    /// Deliberately not generic over arbitrary arity/variadic templates (a fully generic version
    /// would need to cover `std::tuple<Ts...>`/`std::variant<Ts...>` element-by-element, which no
    /// caller in this codebase currently needs) — just the concrete container/wrapper shapes
    /// `shape_of` recognizes as having exactly one or two meaningful argument types. The primary
    /// template is intentionally left with no members: instantiating
    /// `TemplateArgumentsOf<T>::Element` for an unsupported `T` is a compile error at the use site,
    /// which is the desired failure mode (matching how `TypeTraits<T>::name` works for "is this
    /// reflected").
    template <class T>
    struct TemplateArgumentsOf {};

    template <class U, class Alloc>
    struct TemplateArgumentsOf<std::vector<U, Alloc>> {
        using Element = U;
    };

    template <class U, usize N>
    struct TemplateArgumentsOf<std::array<U, N>> {
        using Element = U;
        static constexpr usize Size = N;
    };

    template <class U>
    struct TemplateArgumentsOf<std::optional<U>> {
        using Element = U;
    };

    template <class U>
    struct TemplateArgumentsOf<std::unique_ptr<U>> {
        using Element = U;
    };

    template <class U>
    struct TemplateArgumentsOf<std::shared_ptr<U>> {
        using Element = U;
    };

    template <class U, class Compare, class Alloc>
    struct TemplateArgumentsOf<std::set<U, Compare, Alloc>> {
        using Element = U;
    };

    template <class U, class Hash, class Eq, class Alloc>
    struct TemplateArgumentsOf<std::unordered_set<U, Hash, Eq, Alloc>> {
        using Element = U;
    };

    template <class K, class V, class Compare, class Alloc>
    struct TemplateArgumentsOf<std::map<K, V, Compare, Alloc>> {
        using Key = K;
        using Value = V;
    };

    template <class K, class V, class Hash, class Eq, class Alloc>
    struct TemplateArgumentsOf<std::unordered_map<K, V, Hash, Eq, Alloc>> {
        using Key = K;
        using Value = V;
    };

    template <class A, class B>
    struct TemplateArgumentsOf<std::pair<A, B>> {
        using First = A;
        using Second = B;
    };

} // namespace SFT::Reflection
