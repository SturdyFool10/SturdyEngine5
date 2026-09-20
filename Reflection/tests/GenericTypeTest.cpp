/// Coverage for generic reflection over arbitrary user-defined class templates
/// (`GenericType.hpp`): `ClassTemplateSpecialization`/`is_class_template_specialization`,
/// `template_argument_count`/`TemplateArgument`, the `ClassTemplateName` opt-in mechanism, and
/// `structural_class_template_id`. Every check below is a `static_assert`, mirroring
/// `TypeShapeTest.cpp`'s discipline of proving these are genuinely `consteval`, not just "happens
/// to work at runtime".

#include <Reflection/GenericType.hpp>
#include <Reflection/Macros.hpp>

#include <Foundation/Foundation.hpp>

#include <cstddef>
#include <type_traits>

namespace {

    // ── Shape 1 fixtures: type-only template parameters ────────────────────────────────────────

    template <class T>
    struct Handle {
        T *ptr = nullptr;
    };

    template <class K, class V>
    struct Pair2 {
        K first{};
        V second{};
    };

    // Deliberately NOT opted into ClassTemplateName, to prove structure is still introspectable
    // without a name.
    template <class T>
    struct Unnamed {
        T value{};
    };

    struct Item {
        int id = 0;
    };

    // ── Shape 2 fixture: one type parameter + one trailing non-type parameter ─────────────────

    template <class T, SFT::usize N>
    struct RingBuffer {
        T data[N]{};
    };

} // namespace

SFT_REFLECT_TYPE(Item, "test.reflection.generictype.item");
SFT_REFLECT_FIELD(id);
SFT_REFLECT_END();

namespace SFT::Reflection {
    template <>
    struct ClassTemplateName<Handle> {
        static constexpr std::string_view value = "game.Handle";
    };

    template <>
    struct ClassTemplateName<Pair2> {
        static constexpr std::string_view value = "game.Pair2";
    };
} // namespace SFT::Reflection

using namespace SFT::Reflection;

// ── is_class_template_specialization / ClassTemplateSpecialization ────────────────────────────
static_assert(is_class_template_specialization<Handle<int>>());
static_assert(ClassTemplateSpecialization<Handle<int>>);
static_assert(!is_class_template_specialization<int>());
static_assert(!ClassTemplateSpecialization<int>);
static_assert(!is_class_template_specialization<Item>());

// ── template_argument_count / TemplateArgument: single-type-parameter template ────────────────
static_assert(template_argument_count<Handle<int>>() == 1);
static_assert(std::is_same_v<TemplateArgument<Handle<int>, 0>, int>);
static_assert(!HasNonTypeTemplateArgument<Handle<int>>);

// ── structural_class_template_id: composes a name, and differs across specializations ─────────
static_assert(HasStructuralClassTemplateId<Handle<int>>);
static_assert(structural_class_template_id<Handle<int>>() == TypeId::from_name("game.Handle<i32>"));
static_assert(structural_class_template_id<Handle<int>>() != structural_class_template_id<Handle<float>>());

// ── A reflected struct as the template argument: the composed identity nests Item's own
// canonical (SFT_REFLECT_TYPE) name, mirroring StructuralTypeId.hpp's own std::vector<Item> tests
// ─────────────────────────────────────────────────────────────────────────────────────────────
static_assert(std::is_same_v<TemplateArgument<Handle<Item>, 0>, Item>);
static_assert(HasStructuralClassTemplateId<Handle<Item>>);
static_assert(
    structural_class_template_id<Handle<Item>>() ==
    TypeId::from_name("game.Handle<test.reflection.generictype.item>")
);

// ── Two-type-parameter template: argument extraction + composed identity at arity 2 ────────────
static_assert(template_argument_count<Pair2<int, float>>() == 2);
static_assert(std::is_same_v<TemplateArgument<Pair2<int, float>, 0>, int>);
static_assert(std::is_same_v<TemplateArgument<Pair2<int, float>, 1>, float>);
static_assert(HasStructuralClassTemplateId<Pair2<int, float>>);
static_assert(structural_class_template_id<Pair2<int, float>>() == TypeId::from_name("game.Pair2<i32,f32>"));

// ── Not opted into ClassTemplateName: structure is still fully introspectable, but no
// structural id is available ─────────────────────────────────────────────────────────────────
static_assert(is_class_template_specialization<Unnamed<int>>());
static_assert(template_argument_count<Unnamed<int>>() == 1);
static_assert(std::is_same_v<TemplateArgument<Unnamed<int>, 0>, int>);
static_assert(!HasStructuralClassTemplateId<Unnamed<int>>);

// ── Trailing non-type parameter (std::array<T, N>'s own shape): structural extraction, no name
// ─────────────────────────────────────────────────────────────────────────────────────────────
static_assert(is_class_template_specialization<RingBuffer<int, 4>>());
static_assert(template_argument_count<RingBuffer<int, 4>>() == 1);
static_assert(std::is_same_v<TemplateArgument<RingBuffer<int, 4>, 0>, int>);
static_assert(HasNonTypeTemplateArgument<RingBuffer<int, 4>>);
static_assert(non_type_template_argument<RingBuffer<int, 4>> == 4);
// ClassTemplateName cannot be specialized at all for this shape (template-template-parameter
// kinds don't match `template <class...> class` — see GenericType.hpp's doc comment), so no
// structural id is available for it either, the same as an un-opted-in shape-1 template.
static_assert(!HasStructuralClassTemplateId<RingBuffer<int, 4>>);

int main() {
    // Runtime smoke test mirroring the static_asserts above.
    if (!is_class_template_specialization<Handle<int>>()) {
        return 1;
    }
    if (template_argument_count<Pair2<int, float>>() != 2) {
        return 1;
    }
    if (structural_class_template_id<Handle<Item>>() !=
        TypeId::from_name("game.Handle<test.reflection.generictype.item>")) {
        return 1;
    }
    if (non_type_template_argument<RingBuffer<int, 4>> != 4) {
        return 1;
    }
    return 0;
}
