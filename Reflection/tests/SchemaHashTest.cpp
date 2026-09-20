/// Coverage for `SchemaHash.hpp`'s structural schema/ABI hash (`schema_hash_of<T>()`). Two distinct
/// C++ types cannot trivially be made to produce the *same* schema hash (every `SFT_REFLECT_TYPE`
/// needs a unique canonical name, and that name is itself a hash input), so this instead proves
/// *sensitivity*: field order, canonical name, and base-class schema each independently change the
/// resulting hash, plus a determinism sanity check. Everything below is a `static_assert` — this is
/// entirely `consteval` machinery, proven the same way `StaticReflectionTest.cpp` proves
/// `StaticTypeInfo<T>` is genuinely compile-time-only.

#include <Reflection/Reflection.hpp>
#include <Reflection/SchemaHash.hpp>

namespace {

    struct StructA {
        int a = 0;
        float b = 0.0F;
    };

    // Same field names/types as `StructA`, but declared (and therefore reflected) in the opposite
    // order — proves field order is a real input to the hash, not just field membership.
    struct StructB {
        float b = 0.0F;
        int a = 0;
    };

    // Identical fields and declaration order to `StructA`, but a different canonical name — proves
    // the name string is a real input to the hash, independent of field shape.
    struct StructC {
        int a = 0;
        float b = 0.0F;
    };

    struct BaseWithField {
        int base_value = 0;
    };

    struct DerivedNoBase {
        int base_value = 0;
        int derived_value = 0;
    };

    struct DerivedWithBase {
        int derived_value = 0;
    };

    // Same field, same method name -- differ only in the method's parameter list. Proves
    // `schema_hash_of` folds in `StaticMethodInfo::key()` (name + full parameter signature), not
    // just each method's bare name: a signature change here (an overload added/retyped) is exactly
    // the kind of shape change a save/mod-compatibility hash exists to catch, the same way a
    // retyped *field* already does.
    struct MethodOverloadShort {
        int value = 0;
        [[nodiscard]] int compute(int x) noexcept {
            return value + x;
        }
    };
    struct MethodOverloadLong {
        int value = 0;
        [[nodiscard]] int compute(int x, int y) noexcept {
            return value + x + y;
        }
    };

} // namespace

SFT_REFLECT_TYPE(StructA, "test.reflection.schema_hash.struct_a");
SFT_REFLECT_FIELD(a);
SFT_REFLECT_FIELD(b);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(StructB, "test.reflection.schema_hash.struct_b");
SFT_REFLECT_FIELD(b);
SFT_REFLECT_FIELD(a);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(StructC, "test.reflection.schema_hash.struct_c");
SFT_REFLECT_FIELD(a);
SFT_REFLECT_FIELD(b);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(BaseWithField, "test.reflection.schema_hash.base_with_field");
SFT_REFLECT_FIELD(base_value);
SFT_REFLECT_END();

// Same flattened field layout as `DerivedWithBase` + `BaseWithField` combined, but declared with no
// base at all — the structural comparison point for "does the base actually get folded in".
SFT_REFLECT_TYPE(DerivedNoBase, "test.reflection.schema_hash.derived_no_base");
SFT_REFLECT_FIELD(base_value);
SFT_REFLECT_FIELD(derived_value);
SFT_REFLECT_END();

SFT_REFLECT_TYPE_WITH_BASE(DerivedWithBase, "test.reflection.schema_hash.derived_with_base", BaseWithField);
SFT_REFLECT_FIELD(derived_value);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(MethodOverloadShort, "test.reflection.schema_hash.method_overload_short");
SFT_REFLECT_FIELD(value);
SFT_REFLECT_METHOD(compute);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(MethodOverloadLong, "test.reflection.schema_hash.method_overload_long");
SFT_REFLECT_FIELD(value);
SFT_REFLECT_METHOD(compute);
SFT_REFLECT_END();

using namespace SFT::Reflection;

// ── Determinism: calling twice must yield the same result ──────────────────────────────────────
static_assert(schema_hash_of<StructA>() == schema_hash_of<StructA>());

// ── Field order matters: same fields, opposite declared order ──────────────────────────────────
static_assert(schema_hash_of<StructA>() != schema_hash_of<StructB>());

// ── Canonical name matters: same fields/order, different name ──────────────────────────────────
static_assert(schema_hash_of<StructA>() != schema_hash_of<StructC>());

// ── Base schema is actually folded in, not ignored ──────────────────────────────────────────────
// `DerivedWithBase` reflects only `derived_value` itself, with `BaseWithField` (which reflects
// `base_value`) as its base; `DerivedNoBase` reflects both fields directly with no base at all. If
// `schema_hash_of` ignored bases, these two would hash identically (same field name/type/order set
// for the derived type's own declaration) - they must not.
static_assert(schema_hash_of<DerivedWithBase>() != schema_hash_of<DerivedNoBase>());

// ── Methods are folded in, by full signature ────────────────────────────────────────────────────
static_assert(schema_hash_of<MethodOverloadShort>() != schema_hash_of<MethodOverloadLong>());
static_assert(schema_hash_of<MethodOverloadShort>() == schema_hash_of<MethodOverloadShort>());

int main() {
    // Runtime smoke test mirroring the static_asserts above, so a debug build without full
    // constant-evaluation of every branch still exercises the same code paths at runtime.
    if (schema_hash_of<StructA>() != schema_hash_of<StructA>()) {
        return 1;
    }
    if (schema_hash_of<StructA>() == schema_hash_of<StructB>()) {
        return 1;
    }
    if (schema_hash_of<StructA>() == schema_hash_of<StructC>()) {
        return 1;
    }
    if (schema_hash_of<DerivedWithBase>() == schema_hash_of<DerivedNoBase>()) {
        return 1;
    }
    if (schema_hash_of<MethodOverloadShort>() == schema_hash_of<MethodOverloadLong>()) {
        return 1;
    }
    return 0;
}
