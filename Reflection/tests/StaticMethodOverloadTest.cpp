/// Coverage for the compile-time overload-disambiguation surface added to `StaticReflection.hpp`:
/// `StaticTypeInfo<T>::method_count(name)`, `method_overload<Name, ParamTypes...>()`,
/// `has_method_overload<Name, ParamTypes...>()`, `has_method(name, param_types)`, and
/// `StaticMethodInfo::key()`. Everything below either is a `static_assert` (proving it is
/// genuinely `consteval`, not just `constexpr`-capable) or a runtime mirror of the same checks in
/// `main()`, matching `StaticReflectionTest.cpp`'s convention. This deliberately does not touch
/// `TypeRegistry` at all — the whole point of this file is that the compile-time layer can
/// disambiguate an overload set with zero runtime trace.

#include <Reflection/Reflection.hpp>

namespace {

    // Mirrors `CapabilityTest.cpp`'s `Blaster::take_damage` overload pair (the existing idiom for
    // declaring overloads at the macro layer), plus a third overload differing by parameter type
    // rather than just arity, so `method_overload<...>()` has to actually compare identities and
    // not just arity.
    enum class DamageKind { Physical, Fire };

    struct Blaster {
        int last_amount = 0;
        int last_kind = 0;

        void take_damage(int amount) {
            last_amount = amount;
            last_kind = 1;
        }
        void take_damage(int amount, int kind) {
            last_amount = amount;
            last_kind = kind;
        }
        void take_damage(int amount, DamageKind kind) {
            last_amount = amount;
            last_kind = (kind == DamageKind::Fire) ? 2 : 3;
        }

        // A non-overloaded method, to prove the zero-disambiguation `method<Name>()` path is
        // untouched by any of this file's additions.
        [[nodiscard]] int aim() const noexcept { return last_amount; }
    };

} // namespace

SFT_REFLECT_ENUM(DamageKind, "test.reflection.static_overload.damage_kind");
SFT_REFLECT_ENUM_VALUE(Physical);
SFT_REFLECT_ENUM_VALUE(Fire);
SFT_REFLECT_ENUM_END();

SFT_REFLECT_TYPE(Blaster, "test.reflection.static_overload.blaster");
SFT_REFLECT_FIELD(last_amount);
SFT_REFLECT_FIELD(last_kind);
SFT_REFLECT_METHOD_OVERLOAD(take_damage, void (ReflectedType::*)(int));
SFT_REFLECT_METHOD_OVERLOAD(take_damage, void (ReflectedType::*)(int, int));
SFT_REFLECT_METHOD_OVERLOAD(take_damage, void (ReflectedType::*)(int, DamageKind));
SFT_REFLECT_METHOD(aim);
SFT_REFLECT_END();

using namespace SFT::Reflection;

// ── method_count(name): the size of one specific overload set ──────────────────────────────────
static_assert(reflect<Blaster>().method_count("take_damage") == 3);
static_assert(reflect<Blaster>().method_count("aim") == 1);
static_assert(reflect<Blaster>().method_count("no_such_method") == 0);
// Total across every name must still include all four declared methods.
static_assert(reflect<Blaster>().method_count() == 4);

// ── method_overload<Name, ParamTypes...>(): each overload picked correctly ─────────────────────
static_assert(reflect<Blaster>().method_overload<"take_damage", int>().arity == 1);
static_assert(reflect<Blaster>().method_overload<"take_damage", int>().parameter_types()[0].base == structural_type_id<int>());

static_assert(reflect<Blaster>().method_overload<"take_damage", int, int>().arity == 2);
static_assert(reflect<Blaster>().method_overload<"take_damage", int, int>().parameter_types()[0].base == structural_type_id<int>());
static_assert(reflect<Blaster>().method_overload<"take_damage", int, int>().parameter_types()[1].base == structural_type_id<int>());

static_assert(reflect<Blaster>().method_overload<"take_damage", int, DamageKind>().arity == 2);
static_assert(reflect<Blaster>().method_overload<"take_damage", int, DamageKind>().parameter_types()[1].base == structural_type_id<DamageKind>());

// The two 2-arg overloads must not be confused with each other despite matching on arity alone.
static_assert(reflect<Blaster>().method_overload<"take_damage", int, int>().parameter_types()[1].base !=
              reflect<Blaster>().method_overload<"take_damage", int, DamageKind>().parameter_types()[1].base);

// ── StaticMethodInfo::key(): distinct per overload, deterministic for the same overload ────────
static_assert(reflect<Blaster>().method_overload<"take_damage", int>().key() !=
              reflect<Blaster>().method_overload<"take_damage", int, int>().key());
static_assert(reflect<Blaster>().method_overload<"take_damage", int, int>().key() !=
              reflect<Blaster>().method_overload<"take_damage", int, DamageKind>().key());
static_assert(reflect<Blaster>().method_overload<"take_damage", int>().key() !=
              reflect<Blaster>().method_overload<"take_damage", int, DamageKind>().key());
// Determinism: computing the same overload's key twice must agree.
static_assert(reflect<Blaster>().method_overload<"take_damage", int>().key() ==
              reflect<Blaster>().method_overload<"take_damage", int>().key());
static_assert(reflect<Blaster>().method_overload<"take_damage", int, int>().key() ==
              reflect<Blaster>().method_overload<"take_damage", int, int>().key());

// ── has_method_overload<Name, ParamTypes...>(): existence check, no hard failure ───────────────
static_assert(reflect<Blaster>().has_method_overload<"take_damage", int>());
static_assert(reflect<Blaster>().has_method_overload<"take_damage", int, int>());
static_assert(reflect<Blaster>().has_method_overload<"take_damage", int, DamageKind>());
static_assert(!reflect<Blaster>().has_method_overload<"take_damage", float>());
static_assert(!reflect<Blaster>().has_method_overload<"take_damage">());
static_assert(!reflect<Blaster>().has_method_overload<"no_such_method", int>());

// ── has_method(name, span<const TypeId>): the runtime-span-shaped equivalent ────────────────────
constexpr bool has_method_span_ok = [] {
    const std::array<TypeId, 1> one_arg{structural_type_id<int>()};
    const std::array<TypeId, 2> two_arg{structural_type_id<int>(), structural_type_id<int>()};
    const std::array<TypeId, 2> two_arg_kind{structural_type_id<int>(), structural_type_id<DamageKind>()};
    const std::array<TypeId, 1> wrong_arg{structural_type_id<float>()};
    if (!reflect<Blaster>().has_method("take_damage", std::span<const TypeId>{one_arg})) {
        return false;
    }
    if (!reflect<Blaster>().has_method("take_damage", std::span<const TypeId>{two_arg})) {
        return false;
    }
    if (!reflect<Blaster>().has_method("take_damage", std::span<const TypeId>{two_arg_kind})) {
        return false;
    }
    if (reflect<Blaster>().has_method("take_damage", std::span<const TypeId>{wrong_arg})) {
        return false;
    }
    if (reflect<Blaster>().has_method("no_such_method", std::span<const TypeId>{one_arg})) {
        return false;
    }
    return true;
}();
static_assert(has_method_span_ok);

// ── Regression: the existing zero-disambiguation method<Name>() must be unchanged ──────────────
// `take_damage` is overloaded, so `method<Name>()` (no ParamTypes) must still return the *first*
// declared overload — the 1-arg one, per `SFT_REFLECT_METHOD_OVERLOAD` declaration order above —
// exactly as it did before this file's additions existed.
static_assert(reflect<Blaster>().method<"take_damage">().arity == 1);
static_assert(reflect<Blaster>().method<"take_damage">().parameter_types()[0].base == structural_type_id<int>());
// A non-overloaded method is unaffected either way.
static_assert(reflect<Blaster>().method<"aim">().arity == 0);
static_assert(reflect<Blaster>().has_method("aim"));

int main() {
    // Runtime smoke test mirroring the static_assert checks above, so a debug build without full
    // constant-evaluation of every branch still exercises the same code paths at runtime.
    int failures = 0;

    if (reflect<Blaster>().method_count("take_damage") != 3) {
        ++failures;
    }
    if (reflect<Blaster>().method_overload<"take_damage", int, DamageKind>().arity != 2) {
        ++failures;
    }
    if (reflect<Blaster>().method_overload<"take_damage", int>().key() == reflect<Blaster>().method_overload<"take_damage", int, int>().key()) {
        ++failures;
    }
    if (!reflect<Blaster>().has_method_overload<"take_damage", int, int>()) {
        ++failures;
    }
    if (reflect<Blaster>().has_method_overload<"take_damage", float>()) {
        ++failures;
    }
    if (reflect<Blaster>().method<"take_damage">().arity != 1) {
        ++failures;
    }

    return failures == 0 ? 0 : 1;
}
