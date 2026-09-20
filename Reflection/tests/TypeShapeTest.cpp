/// Coverage for the generic type-shape classifier (`TypeShape.hpp`): `shape_of<T>()` and
/// `TemplateArgumentsOf<T>`. Every check below is a `static_assert`, proving `shape_of` is
/// genuinely `consteval` and that `TemplateArgumentsOf`'s member aliases resolve as expected —
/// none of this touches `TypeRegistry`, mirroring `StaticReflectionTest.cpp`'s discipline.

// `Macros.hpp` is included explicitly rather than relied on transitively through
// `TypeShape.hpp`: the container-detection traits those two share now live in the
// dependency-free `ContainerTraits.hpp`, so `TypeShape.hpp` no longer drags in the
// SFT_REFLECT_* macros this file uses to declare its own reflected fixtures.
#include <Reflection/Macros.hpp>
#include <Reflection/TypeShape.hpp>

#include <Foundation/Foundation.hpp>

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

    struct Item {
        int id = 0;
    };

    enum class Color { Red, Green, Blue };

} // namespace

SFT_REFLECT_TYPE(Item, "test.reflection.typeshape.item");
SFT_REFLECT_FIELD(id);
SFT_REFLECT_END();

using namespace SFT::Reflection;

// ── Sequence / Array ────────────────────────────────────────────────────────────────────────────
static_assert(shape_of<std::vector<int>>() == TypeShape::Sequence);
static_assert(std::is_same_v<TemplateArgumentsOf<std::vector<int>>::Element, int>);

static_assert(shape_of<std::array<int, 4>>() == TypeShape::Array);
static_assert(std::is_same_v<TemplateArgumentsOf<std::array<int, 4>>::Element, int>);
static_assert(TemplateArgumentsOf<std::array<int, 4>>::Size == 4);

// ── Struct: a vector of a reflected element type is still just a Sequence; the *element*
// classifies as Struct, not the vector itself ───────────────────────────────────────────────────
static_assert(shape_of<std::vector<Item>>() == TypeShape::Sequence);
static_assert(std::is_same_v<TemplateArgumentsOf<std::vector<Item>>::Element, Item>);
static_assert(shape_of<Item>() == TypeShape::Struct);

// ── Associative ──────────────────────────────────────────────────────────────────────────────────
// Deliberately keyed by `int`, not `UString`/`std::string` — the audit's literal
// `unordered_map<string, Item>` example needs `Foundation::UString` added to
// `StaticTypeId.hpp`'s fundamental-type table before a string-keyed map's `TypeId` can be derived
// (out of scope here: this file must not edit `StaticTypeId.hpp`). `shape_of`/`TemplateArgumentsOf`
// themselves are keyed on template shape, not on `TypeId`-ability, so this still fully proves the
// classification and argument-extraction for `unordered_map`.
static_assert(shape_of<std::unordered_map<int, Item>>() == TypeShape::Associative);
static_assert(std::is_same_v<TemplateArgumentsOf<std::unordered_map<int, Item>>::Key, int>);
static_assert(std::is_same_v<TemplateArgumentsOf<std::unordered_map<int, Item>>::Value, Item>);

static_assert(shape_of<std::map<int, Item>>() == TypeShape::Associative);
static_assert(std::is_same_v<TemplateArgumentsOf<std::map<int, Item>>::Key, int>);
static_assert(std::is_same_v<TemplateArgumentsOf<std::map<int, Item>>::Value, Item>);

// ── Set ──────────────────────────────────────────────────────────────────────────────────────────
static_assert(shape_of<std::set<int>>() == TypeShape::Set);
static_assert(std::is_same_v<TemplateArgumentsOf<std::set<int>>::Element, int>);
static_assert(shape_of<std::unordered_set<int>>() == TypeShape::Set);

// ── Optional / SmartPointer ─────────────────────────────────────────────────────────────────────
static_assert(shape_of<std::optional<int>>() == TypeShape::Optional);
static_assert(std::is_same_v<TemplateArgumentsOf<std::optional<int>>::Element, int>);

static_assert(shape_of<std::unique_ptr<int>>() == TypeShape::SmartPointer);
static_assert(std::is_same_v<TemplateArgumentsOf<std::unique_ptr<int>>::Element, int>);
static_assert(shape_of<std::shared_ptr<int>>() == TypeShape::SmartPointer);

// ── String / Enum / Primitive / Pointer / Reference ────────────────────────────────────────────
static_assert(shape_of<SFT::Foundation::UString>() == TypeShape::String);
static_assert(shape_of<std::string>() == TypeShape::String);
static_assert(shape_of<std::string_view>() == TypeShape::String);

static_assert(shape_of<Color>() == TypeShape::Enum);

static_assert(shape_of<int>() == TypeShape::Primitive);
static_assert(shape_of<bool>() == TypeShape::Primitive);
static_assert(shape_of<float>() == TypeShape::Primitive);

static_assert(shape_of<int *>() == TypeShape::Pointer);
static_assert(shape_of<const int *>() == TypeShape::Pointer);

static_assert(shape_of<int &>() == TypeShape::Reference);
static_assert(shape_of<int &&>() == TypeShape::Reference);
static_assert(shape_of<Item &>() == TypeShape::Reference);

// ── Tuple / Variant / Function ──────────────────────────────────────────────────────────────────
static_assert(shape_of<std::pair<int, float>>() == TypeShape::Tuple);
static_assert(std::is_same_v<TemplateArgumentsOf<std::pair<int, float>>::First, int>);
static_assert(std::is_same_v<TemplateArgumentsOf<std::pair<int, float>>::Second, float>);

static_assert(shape_of<std::tuple<int, float, bool>>() == TypeShape::Tuple);
static_assert(shape_of<std::variant<int, float>>() == TypeShape::Variant);

static_assert(shape_of<void (*)(int)>() == TypeShape::Function);
static_assert(shape_of<std::function<void(int)>>() == TypeShape::Function);

// ── Unknown ──────────────────────────────────────────────────────────────────────────────────────
struct NotReflected {
    int value = 0;
};
static_assert(shape_of<NotReflected>() == TypeShape::Unknown);

int main() {
    // Runtime smoke test mirroring the static_asserts above, so a debug build without full
    // constant-evaluation of every branch still exercises the same code paths at runtime.
    if (shape_of<std::vector<Item>>() != TypeShape::Sequence) {
        return 1;
    }
    if (shape_of<std::unordered_map<int, Item>>() != TypeShape::Associative) {
        return 1;
    }
    if (shape_of<Item>() != TypeShape::Struct) {
        return 1;
    }
    if (shape_of<SFT::Foundation::UString>() != TypeShape::String) {
        return 1;
    }
    return 0;
}
