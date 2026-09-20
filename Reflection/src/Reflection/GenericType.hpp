#pragma once

#include <Reflection/StaticTypeId.hpp>
#include <Reflection/StructuralTypeId.hpp>
#include <Reflection/TypeId.hpp>
#include <Reflection/TypeRef.hpp>
#include <Reflection/TypeShape.hpp>

#include <Foundation/Foundation.hpp>

#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

/// Generic reflection over arbitrary USER-DEFINED class templates — a game's own
/// `template <class T> class Handle { ... }` or `template <class T, usize N> class RingBuffer`
/// — as opposed to `TypeShape.hpp`'s `TemplateArgumentsOf`, which only recognizes a closed set of
/// specific standard-library shapes (`std::vector`, `std::optional`, `std::map`, ...) via one
/// explicit specialization per shape. That closed-set design cannot scale to "any template a game
/// happens to write," since this package has no way to know about a template it has never seen —
/// this file exists to close that gap with a *pattern-matching* mechanism instead of a lookup
/// table: class template argument deduction in a partial specialization, which lets a single
/// specialization match every specialization of every class template with a given parameter-list
/// shape, whatever the template is actually called.
///
/// Two separate questions are deliberately kept separate, mirroring `TypeShape.hpp`'s own split
/// between `shape_of` (always answerable) and `TemplateArgumentsOf` (only for recognized shapes):
///   - "What is this type's *structure*?" (is it a class template specialization, how many
///     template arguments does it have, what are they) — always answerable for the supported
///     parameter-list shapes (see below), with no per-template opt-in required.
///   - "What is this type's *canonical name*?" — only answerable for a class template the game has
///     explicitly opted into naming via `ClassTemplateName` (see below), since C++ has no portable
///     way to recover a class template's own name as a string without RTTI/compiler-specific
///     tricks (`__PRETTY_FUNCTION__` and friends), which this package's entire identity model
///     forbids for the same reason `StaticTypeId.hpp` forbids `typeid(T).name()`: an identity that
///     can silently change across compilers/builds is worse than no identity at all.
///
/// ── Supported parameter-list shapes (empirically verified, not assumed) ───────────────────────
///
/// 1. Any arity of *type-only* template parameters — `Handle<T>`, `Pair2<K, V>`,
///    `Triple<A, B, C>`, etc. Matched by deducing against `TT<Args...>` where `TT` is a
///    `template <class...> class` template-template-parameter. This is the shape
///    `ClassTemplateName` can also assign a canonical name to (see below), because a
///    `template <class...> class` template-template-parameter is itself the shape used to declare
///    `ClassTemplateName`'s own parameter.
///
/// 2. Exactly one type parameter followed by exactly one trailing non-type parameter —
///    `RingBuffer<T, N>`, mirroring `std::array<T, N>`'s own shape (which is why
///    `TypeShape.hpp`'s `TemplateArgumentsOf<std::array<U, N>>` needed its own explicit
///    specialization rather than falling out of a fully generic one). Matched by deducing against
///    `TT<Arg, NonTypeArg>` where `TT` is a `template <class, auto> class`
///    template-template-parameter. The *structure* (argument count, the type argument itself, the
///    non-type argument's value) is fully introspectable for this shape. Its *name* is NOT
///    supportable through `ClassTemplateName`: a `template <class, auto> class` template-template-
///    parameter is a different kind from `template <class...> class`, and attempting to specialize
///    `ClassTemplateName<RingBuffer>` against the `template <class...> class` form is a hard
///    compile error ("template template argument has different template parameters than its
///    corresponding template template parameter") — verified directly against this compiler, not
///    assumed. A class template of this shape therefore has full structural introspection but no
///    canonical name via this mechanism.
///
/// Anything else — two or more type parameters *and* a trailing non-type parameter
/// (`template <class, class, auto> class`), a leading non-type parameter, multiple non-type
/// parameters, or a template-template parameter used as an argument — falls through to the
/// primary template (`is_specialization == false`). Extending coverage to those shapes would need
/// additional partial specializations of `Detail::ClassTemplateArgumentsOf`, deliberately left out
/// of this pass: no caller in this codebase needs them yet, and each additional shape is a
/// combinatorial addition that should be justified by an actual use site rather than spec'd
/// speculatively.
namespace SFT::Reflection {

    namespace Detail {

        /// Primary template: "not a class template specialization (of any shape this file
        /// recognizes)". Deliberately never removed/hidden for a `T` that fails to match either
        /// partial specialization below — falling through here (rather than to a hard compile
        /// error) is what lets `is_class_template_specialization<T>()`/`ClassTemplateSpecialization`
        /// answer `false` for an ordinary, non-template type instead of failing to compile.
        template <class T>
        struct ClassTemplateArgumentsOf {
            static constexpr bool is_specialization = false;
            static constexpr usize count = 0;
            static constexpr bool has_non_type_argument = false;
        };

        /// Shape 1: any arity of type-only template parameters. `TT` is deduced as the class
        /// template itself (as a template-template-argument), `Args...` as its type arguments —
        /// this is genuine class template argument deduction in a partial specialization, not a
        /// lookup: it matches `Handle<Player>`, `Pair2<i32, Item>`, or any other specialization of
        /// any template shaped like `template <class...> class`, without this file having to know
        /// the template's name in advance.
        template <template <class...> class TT, class... Args>
        struct ClassTemplateArgumentsOf<TT<Args...>> {
            static constexpr bool is_specialization = true;
            static constexpr usize count = sizeof...(Args);
            static constexpr bool has_non_type_argument = false;
            using Arguments = std::tuple<Args...>;
        };

        /// Shape 2: exactly one type parameter followed by one trailing non-type parameter, the
        /// `std::array<T, N>` shape. A second, separate partial specialization is required for
        /// this — shape 1's `template <class...> class TT` deduction never matches here, since `N`
        /// is not a type and therefore cannot appear in a `class...` pack (verified: attempting to
        /// deduce `TT<Args...>` against e.g. `RingBuffer<i32, 4>` fails and falls through to the
        /// primary template, it does not error).
        template <template <class, auto> class TT, class Arg, auto NonTypeArg>
        struct ClassTemplateArgumentsOf<TT<Arg, NonTypeArg>> {
            static constexpr bool is_specialization = true;
            static constexpr usize count = 1;
            static constexpr bool has_non_type_argument = true;
            using Arguments = std::tuple<Arg>;
            static constexpr auto non_type_argument = NonTypeArg;
        };

    } // namespace Detail

    /// True when `T` (after `remove_cv`) is a specialization of some class template whose
    /// parameter-list shape this file recognizes (see the file-level doc comment for exactly which
    /// shapes). `false` for anything else, including ordinary non-template types — this concept is
    /// total, it never fails to compile for a given `T`.
    template <class T>
    concept ClassTemplateSpecialization = Detail::ClassTemplateArgumentsOf<std::remove_cv_t<T>>::is_specialization;

    /// Reports whether `T` is a specialization of some user-defined (or any other) class template,
    /// for one of the parameter-list shapes this file recognizes.
    ///
    /// @return Returns `true` when `T` matches a recognized class-template-specialization shape.
    /// @note This function does not throw exceptions.
    template <class T>
    [[nodiscard]] consteval bool is_class_template_specialization() noexcept {
        return ClassTemplateSpecialization<T>;
    }

    /// The number of template arguments `T`'s class template was specialized with, counting only
    /// the way this file's supported shapes count them: a type-only specialization counts every
    /// type argument, while the "one type + trailing non-type" shape counts just the one type
    /// argument (the non-type argument is exposed separately — see `non_type_template_argument`).
    ///
    /// @return Returns the newly counted total.
    /// @note This function does not throw exceptions.
    template <class T>
        requires ClassTemplateSpecialization<T>
    [[nodiscard]] consteval usize template_argument_count() noexcept {
        return Detail::ClassTemplateArgumentsOf<std::remove_cv_t<T>>::count;
    }

    /// True when `T` is a class template specialization of the "one type + trailing non-type
    /// parameter" shape (`RingBuffer<T, N>`, mirroring `std::array<T, N>`) — the only recognized
    /// shape that carries a non-type template argument.
    template <class T>
    concept HasNonTypeTemplateArgument =
        ClassTemplateSpecialization<T> && Detail::ClassTemplateArgumentsOf<std::remove_cv_t<T>>::has_non_type_argument;

    /// The `I`th type template argument `T`'s class template was specialized with. For the
    /// "type + trailing non-type" shape, `I` must be `0` (the sole type argument); the non-type
    /// argument itself is not indexable through this alias, see `non_type_template_argument`.
    template <class T, usize I>
        requires ClassTemplateSpecialization<T> && (I < Detail::ClassTemplateArgumentsOf<std::remove_cv_t<T>>::count)
    using TemplateArgument =
        std::tuple_element_t<I, typename Detail::ClassTemplateArgumentsOf<std::remove_cv_t<T>>::Arguments>;

    /// The non-type template argument's value, for a `T` matching the "type + trailing non-type"
    /// shape (see `HasNonTypeTemplateArgument`). A variable template rather than a function since
    /// the non-type argument's own type varies per specialization (`RingBuffer<T, N>`'s `N` might
    /// be `usize`, `int`, an enum, ...) — a uniformly-typed `consteval` function couldn't return it
    /// without erasing that type.
    template <class T>
        requires HasNonTypeTemplateArgument<T>
    inline constexpr auto non_type_template_argument = Detail::ClassTemplateArgumentsOf<std::remove_cv_t<T>>::non_type_argument;

    /// A class template's own canonical, opt-in name — e.g.
    /// `template <> struct ClassTemplateName<Handle> { static constexpr std::string_view value =
    /// "game.Handle"; };`, specialized once per template family by whoever owns it (typically
    /// right next to the template's own definition, the same place `SFT_REFLECT_TYPE` would sit
    /// for an ordinary struct).
    ///
    /// This exists only because C++ has no portable way to recover a class template's own name as
    /// a string: `__PRETTY_FUNCTION__`/`typeid`-based extraction is compiler- and build-dependent
    /// (see `StaticTypeId.hpp`'s doc comments on exactly this issue for ordinary types), which this
    /// package's canonical-identity model forbids outright. The primary template's `value` is
    /// empty, meaning "not opted in" — an un-opted-in template has no canonical name through this
    /// mechanism, but its *structure* (arity, argument types) remains fully introspectable via
    /// `ClassTemplateSpecialization`/`TemplateArgument` regardless (see the file-level doc comment
    /// for this split).
    ///
    /// Only usable for the "type-only parameters" shape (`template <class...> class`) —
    /// `ClassTemplateName<TT>` cannot be specialized for a `template <class, auto> class` template
    /// like the `std::array`-shaped `RingBuffer<T, N>` at all: the template-template-parameter
    /// kinds don't match, and the attempt is a hard compile error, not a silent SFINAE failure
    /// (empirically verified against this compiler). A game wanting a canonical name for a
    /// `std::array`-shaped template has no option here but to give it a differently-shaped
    /// signature (e.g. wrap the extent in a type, or accept only the "type-only" limitation).
    template <template <class...> class TT>
    struct ClassTemplateName {
        static constexpr std::string_view value{};
    };

    namespace Detail {

        /// True when `TT` has opted into `ClassTemplateName`.
        template <template <class...> class TT>
        concept HasClassTemplateName = !ClassTemplateName<TT>::value.empty();

        /// Deduces `TT`/`Args...` straight from a `TT<Args...> *` argument (rather than requiring a
        /// second partial specialization of some helper struct) so the `requires` clause below can
        /// check *both* "has `TT` opted into `ClassTemplateName`" and "is every `Args` itself
        /// structurally nameable" in one place, and so overload resolution — not a hard error —
        /// is what happens when either check fails (letting `HasStructuralClassTemplateId` below
        /// detect support with a `requires` expression instead of hard-failing the moment a caller
        /// names this function for an unsupported `T`).
        ///
        /// Composes the name by calling straight into `StructuralTypeId.hpp`'s own
        /// `Detail::append_structural_type_name` for each argument — the exact routine
        /// `structural_type_id<Arg>()` itself is built on — rather than re-deriving a name from
        /// `structural_type_id<Arg>()`'s `TypeId` result (which is a one-way FNV-1a128 hash; there
        /// is no name to recover from it). This is "reuse the existing composition logic, don't
        /// reimplement it" applied to the one signature that's actually capable of doing so.
        template <template <class...> class TT, class... Args>
            requires HasClassTemplateName<TT> && (StructurallyIdentifiable<Args> && ...)
        [[nodiscard]] consteval TypeId structural_class_template_id_impl(TT<Args...> *) noexcept {
            std::string out;
            out.append(ClassTemplateName<TT>::value);
            out.push_back('<');
            bool first = true;
            ((out.append(first ? "" : ","), first = false, Detail::append_structural_type_name<Args>(out)), ...);
            out.push_back('>');
            return TypeId::from_name(out);
        }

    } // namespace Detail

    /// Reports whether `structural_class_template_id<T>()` accepts `T`: `T` must be a "type-only
    /// parameters" class template specialization (shape 1, see the file-level doc comment) whose
    /// own template has opted into `ClassTemplateName`, and every one of its type arguments must
    /// itself be `StructurallyIdentifiable` (`StructuralTypeId.hpp`) so the composed name is
    /// well-formed all the way down.
    template <class T>
    concept HasStructuralClassTemplateId = requires(std::remove_cv_t<T> *ptr) {
        Detail::structural_class_template_id_impl(ptr);
    };

    /// `T`'s composed canonical identity as a class template specialization — e.g.
    /// `game.Handle<game.Player>` for `Handle<Player>`, given `Handle` opted into
    /// `ClassTemplateName` as `"game.Handle"` — following `StructuralTypeId.hpp`'s exact philosophy
    /// of building a name out of parts rather than requiring `T` to have a name of its own. See
    /// `HasStructuralClassTemplateId` for exactly which `T` this accepts.
    ///
    /// @return Returns the newly constructed id.
    /// @note This function does not throw exceptions.
    template <class T>
        requires HasStructuralClassTemplateId<T>
    [[nodiscard]] consteval TypeId structural_class_template_id() noexcept {
        return Detail::structural_class_template_id_impl(static_cast<std::remove_cv_t<T> *>(nullptr));
    }

    /// The `TypeRef`-producing equivalent of `structural_class_template_id<T>()`, preserving `T`'s
    /// exact cv/ref/pointer qualifiers the same way `structural_type_ref<T>()`
    /// (`StructuralTypeId.hpp`) and `type_ref<T>()` (`StaticTypeId.hpp`) do.
    ///
    /// @return Returns the newly constructed reference.
    /// @note This function does not throw exceptions.
    template <class T>
        requires HasStructuralClassTemplateId<std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>>
    [[nodiscard]] consteval TypeRef structural_class_template_ref() noexcept {
        using Bare = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
        return TypeRef{.base = structural_class_template_id<Bare>(), .qualifiers = Detail::qualifiers_of<T>()};
    }

} // namespace SFT::Reflection
