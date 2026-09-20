/// Coverage for the compile-time-only enum layer (`StaticEnum.hpp`): `reflect_enum<E>()`,
/// scoped/unscoped detection, underlying type, enumerator enumeration, `contains`/`name_of`/
/// `value_of`, alias handling, and the runtime-name lookup escape hatch. Every check that can be a
/// `static_assert` is one — proving these resolve during compilation and leave no runtime trace,
/// which is the entire point of having this alongside the runtime `EnumInfo`/`TypeRegistry` path.

#include <Reflection/Reflection.hpp>
#include <Reflection/StaticEnum.hpp>

namespace {

    /// `Favorite` deliberately aliases `Blue` — C++ permits two enumerators with one value, and a
    /// reflection layer that silently dropped or mis-resolved aliases would corrupt round trips.
    enum class Color : SFT::u8 {
        Red = 1,
        Green = 2,
        Blue = 4,
        Favorite = 4,
    };

    /// Unscoped, to exercise `is_scoped()`'s discrimination.
    enum Legacy {
        Alpha = 0,
        Beta = 1,
    };

} // namespace

SFT_REFLECT_ENUM(Color, "test.reflection.static_enum_layer.color");
SFT_REFLECT_ENUM_VALUE(Red);
SFT_REFLECT_ENUM_VALUE(Green);
SFT_REFLECT_ENUM_VALUE(Blue);
SFT_REFLECT_ENUM_VALUE(Favorite);
SFT_REFLECT_ENUM_END();

SFT_REFLECT_ENUM(Legacy, "test.reflection.static_enum_layer.legacy");
SFT_REFLECT_ENUM_VALUE(Alpha);
SFT_REFLECT_ENUM_VALUE(Beta);
SFT_REFLECT_ENUM_END();

using namespace SFT::Reflection;

// ── Identity and shape ────────────────────────────────────────────────────────────────────────
static_assert(reflect_enum<Color>().name() == "test.reflection.static_enum_layer.color");
static_assert(reflect_enum<Color>().type_id() == type_id<Color>());
static_assert(reflect_enum<Color>().underlying_type() == type_id<SFT::u8>());
static_assert(reflect_enum<Color>().enumerator_count() == 4);
static_assert(reflect_enum<Color>().is_scoped());
static_assert(!reflect_enum<Legacy>().is_scoped());

// ── Enumerator access ─────────────────────────────────────────────────────────────────────────
static_assert(reflect_enum<Color>().enumerators()[0].name == "Red");
static_assert(reflect_enum<Color>().enumerators()[0].value == 1);
static_assert(reflect_enum<Color>().name_of(Color::Green) == "Green");
static_assert(reflect_enum<Color>().value_of<"Blue">() == Color::Blue);

// ── contains(): the check a deserializer needs, since any integer can be cast to an enum ──────
static_assert(reflect_enum<Color>().contains(Color::Red));
static_assert(!reflect_enum<Color>().contains(static_cast<Color>(99)));
static_assert(reflect_enum<Color>().contains("Green"));
static_assert(!reflect_enum<Color>().contains("Purple"));

// ── Aliases: every declaration is listed; name_of resolves to the first declared ──────────────
static_assert(reflect_enum<Color>().enumerators()[3].name == "Favorite");
static_assert(reflect_enum<Color>().enumerators()[3].value == 4);
static_assert(reflect_enum<Color>().name_of(Color::Favorite) == "Blue");
static_assert(reflect_enum<Color>().value_of<"Favorite">() == Color::Blue);

// ── Name-as-data lookup (console command, save file): sentinel-returning, not a compile error ──
constexpr bool try_value_of_ok = [] {
    Color resolved{};
    if (!reflect_enum<Color>().try_value_of("Green", resolved) || resolved != Color::Green) {
        return false;
    }
    return !reflect_enum<Color>().try_value_of("Nope", resolved);
}();
static_assert(try_value_of_ok);

int main() {
    // Runtime mirror, so the same paths are exercised even where the compiler did not have to
    // constant-evaluate them, and so the static layer can be cross-checked against the runtime
    // EnumInfo the registry builds from the very same EnumTraits walk.
    const EnumInfo &runtime_enum = TypeRegistry::instance().enum_type<Color>();
    if (runtime_enum.key != reflect_enum<Color>().type_id()) {
        return 1;
    }
    if (runtime_enum.enumerators.size() != reflect_enum<Color>().enumerator_count()) {
        return 1;
    }
    if (runtime_enum.underlying_type != reflect_enum<Color>().underlying_type()) {
        return 1;
    }
    Color resolved{};
    if (!reflect_enum<Color>().try_value_of("Blue", resolved) || resolved != Color::Blue) {
        return 1;
    }
    return 0;
}
