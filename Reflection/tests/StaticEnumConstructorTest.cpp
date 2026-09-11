/// Coverage for static fields/methods, parameterized constructors, and enum reflection.

#include <array>
#include <cstdio>
#include <new>

#include <Reflection/Reflection.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "StaticEnumConstructorTest: %s\n", description);
            ++failures;
        }
    }

    enum class Color : u8 {
        Red = 0,
        Green = 1,
        Blue = 2,
    };

    struct Widget {
        static int instance_count;
        int value = 0;

        Widget() = default;
        Widget(int initial_value, Color color) noexcept : value(initial_value + static_cast<int>(color)) {
            ++instance_count;
        }

        static int double_value(int value) noexcept {
            return value * 2;
        }
    };

    int Widget::instance_count = 0;

} // namespace

SFT_REFLECT_ENUM(Color, "test.reflection.static_enum.color");
SFT_REFLECT_ENUM_VALUE(Red);
SFT_REFLECT_ENUM_VALUE(Green);
SFT_REFLECT_ENUM_VALUE(Blue);
SFT_REFLECT_ENUM_END();

SFT_REFLECT_TYPE(Widget, "test.reflection.static_enum.widget");
SFT_REFLECT_FIELD(value);
SFT_REFLECT_STATIC_FIELD(instance_count, SFT_ATTR_STRING("purpose", "diagnostic counter"));
SFT_REFLECT_STATIC_METHOD(double_value);
SFT_REFLECT_CONSTRUCTOR(int, Color);
SFT_REFLECT_END();

namespace {

    void reflected_double_value_override(void *, const void *const *args, void *out_return, void *user_data) noexcept {
        auto *calls = static_cast<int *>(user_data);
        ++(*calls);
        const int input = *static_cast<const int *>(args[0]);
        if (out_return != nullptr) {
            *static_cast<int *>(out_return) = input * 100;
        }
    }

} // namespace

int main() {
    using namespace SFT::Reflection;

    TypeRegistry &registry = TypeRegistry::instance();
    const TypeInfo &type = registry.type<Widget>();

    // ── Static fields ──────────────────────────────────────────────────────────────────────────
    const FieldInfo *count_field = type.find_field("instance_count");
    check(count_field != nullptr, "instance_count must be found via the normal find_field path");
    check(count_field != nullptr && has_flag(count_field->flags, FieldFlags::Static),
          "instance_count must be flagged Static");
    check(count_field != nullptr && has_flag(count_field->flags, FieldFlags::Trivial),
          "an int static field must still be Trivial");
    check(count_field != nullptr && find_attribute(*count_field, "purpose") != nullptr,
          "a static field must carry its declared attributes");

    Widget::instance_count = 5;
    int read_count = 0;
    check(count_field != nullptr && copy_static_field_out(*count_field, &read_count, sizeof(read_count)),
          "reading a static field must succeed");
    check(read_count == 5, "reading a static field must reflect the live static value");

    const int new_count = 42;
    check(count_field != nullptr && copy_static_field_in(*count_field, &new_count, sizeof(new_count)),
          "writing a static field must succeed");
    check(Widget::instance_count == 42, "writing a static field must land in the actual static storage");

    const FieldInfo *value_field = type.find_field("value");
    check(value_field != nullptr && !has_flag(value_field->flags, FieldFlags::Static),
          "an ordinary instance field must not be flagged Static");

    // ── Static methods: invocation, overrides, and hooks all reuse the instance-method machinery ──
    const MethodInfo *double_value_method = type.find_method("double_value");
    check(double_value_method != nullptr, "double_value must be found via the normal find_method path");
    check(double_value_method != nullptr && double_value_method->is_static, "double_value must be flagged static");

    int amount = 21;
    const void *args[]{&amount};
    int result = 0;
    check(double_value_method != nullptr && invoke_static_method(*double_value_method, args, 1, &result),
          "invoking a static method must succeed");
    check(result == 42, "invoking a static method must run the real implementation");

    int override_calls = 0;
    check(double_value_method != nullptr &&
              registry.set_method_override(type.key, double_value_method->key, &reflected_double_value_override, &override_calls),
          "installing an override on a static method must succeed (same API as instance methods)");
    result = 0;
    check(double_value_method != nullptr && invoke_static_method(*double_value_method, args, 1, &result),
          "invoking an overridden static method must succeed");
    check(result == 2100, "an installed override must run instead of the real static implementation");
    check(override_calls == 1, "the override must have been called exactly once");
    check(double_value_method != nullptr && registry.clear_method_override(type.key, double_value_method->key),
          "clearing the override on a static method must succeed");

    // ── Parameterized constructors ────────────────────────────────────────────────────────────
    const std::array<TypeId, 2> ctor_params{type_id_for<int>(), type_id_for<Color>()};
    const ConstructorInfo *ctor = type.find_constructor(ctor_params);
    check(ctor != nullptr, "a constructor matching (int, Color) must be found");

    Widget::instance_count = 0;
    alignas(Widget) unsigned char storage[sizeof(Widget)];
    int ctor_arg0 = 10;
    Color ctor_arg1 = Color::Blue;
    const void *ctor_args[]{&ctor_arg0, &ctor_arg1};
    check(ctor != nullptr && construct_instance(*ctor, storage, ctor_args, 2),
          "invoking a parameterized constructor must succeed");
    auto *constructed = std::launder(reinterpret_cast<Widget *>(storage));
    check(constructed->value == 12, "a parameterized constructor must run the real constructor body");
    check(Widget::instance_count == 1, "a parameterized constructor's side effects must actually occur");
    constructed->~Widget();

    const std::array<TypeId, 1> wrong_ctor_params{type_id_for<float>()};
    check(type.find_constructor(wrong_ctor_params) == nullptr,
          "find_constructor must miss a signature that was never declared");

    // ── Enums ──────────────────────────────────────────────────────────────────────────────────
    const EnumInfo &color_type = registry.enum_type<Color>();
    check(color_type.canonical_name.cpp_string_view() == "test.reflection.static_enum.color",
          "enum canonical name must round-trip");
    check(color_type.enumerators.size() == 3, "all three enumerators must be reflected");
    check(color_type.underlying_type == type_id_for<u8>(), "the enum's underlying type must be reflected correctly");

    const EnumeratorInfo *green_by_name = color_type.find_enumerator("Green");
    check(green_by_name != nullptr && green_by_name->value == 1, "an enumerator must be found by name with the right value");

    const EnumeratorInfo *blue_by_value = color_type.find_enumerator(static_cast<i64>(2));
    check(blue_by_value != nullptr && blue_by_value->name.cpp_string_view() == "Blue",
          "an enumerator must be found by value with the right name");

    check(color_type.find_enumerator("DoesNotExist") == nullptr, "find_enumerator must miss an undeclared name");
    check(registry.find_enum(color_type.key) == &color_type, "find_enum(TypeId) must return the same descriptor");
    check(registry.find_enum(color_type.canonical_name) == &color_type, "find_enum(name) must return the same descriptor");

    if (failures != 0) {
        (void)std::fprintf(stderr, "StaticEnumConstructorTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
