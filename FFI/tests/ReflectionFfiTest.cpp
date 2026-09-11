/// Exercises the reflection/modding C ABI end to end, entirely through `sturdy_reflection_*` —
/// no `SturdyEngine` handle, no window, no graphics device, since `TypeRegistry` is a
/// process-wide singleton these functions resolve directly against. Covers both halves of the
/// surface: reflecting an existing C++ type (`SFT_REFLECT_TYPE`) and building one entirely at
/// runtime with no C++ type behind it at all (`sturdy_reflection_type_builder_*`), the way a
/// non-C++ mod with no compiled struct of its own would.

#include <cstdio>
#include <cstring>

#include <Reflection/Reflection.hpp>

#include <FFI/Sturdy.h>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "ReflectionFfiTest: %s\n", description);
            ++failures;
        }
    }

    struct Widget {
        int health = 100;

        int take_damage(int amount) noexcept {
            health -= amount;
            return health;
        }
    };

    void override_take_damage(void *object, const void *const *args, void *out_return, void *user_data) {
        auto *calls = static_cast<int *>(user_data);
        ++(*calls);
        auto *widget = static_cast<Widget *>(object);
        const int amount = *static_cast<const int *>(args[0]);
        widget->health -= amount * 2;
        if (out_return != nullptr) {
            *static_cast<int *>(out_return) = widget->health;
        }
    }

    void before_hook(void * /*object*/, const void *const * /*args*/, void *user_data) {
        ++(*static_cast<int *>(user_data));
    }

    void event_listener(void * /*object*/, const void *const *args, void *user_data) {
        auto *received = static_cast<int *>(user_data);
        *received = *static_cast<const int *>(args[0]);
    }

    void dynamic_field_get(const void *object, void *out_value) {
        std::memcpy(out_value, object, sizeof(int));
    }

    void dynamic_field_set(void *object, const void *in_value) {
        std::memcpy(object, in_value, sizeof(int));
    }

    void dynamic_default_construct(void *destination, void * /*user_data*/) {
        *static_cast<int *>(destination) = 0;
    }

    void dynamic_destroy(void * /*object*/, void * /*user_data*/) {}

    void dynamic_move_construct(void *destination, void *source, void * /*user_data*/) {
        *static_cast<int *>(destination) = *static_cast<int *>(source);
    }

} // namespace

SFT_REFLECT_TYPE(Widget, "test.ffi.reflection.widget");
SFT_REFLECT_FIELD(health);
SFT_REFLECT_METHOD(take_damage);
SFT_REFLECT_EVENT("on_damaged", int);
SFT_REFLECT_END();

int main() {
    // SFT_REFLECT_TYPE only specializes TypeTraits<Widget> at compile time — it does not
    // register anything (see Macros.hpp's doc comments). A foreign caller has no way to trigger
    // that registration itself: try_register<T>()/type<T>() are C++ templates requiring
    // compile-time knowledge of T, which is exactly what a foreign caller does not have. So some
    // C++ code in the same process — game/engine startup code, not this ABI — must call
    // TypeRegistry::instance().type<Widget>() (or try_register<Widget>()) at least once before a
    // mod can find "test.ffi.reflection.widget" through sturdy_reflection_find_type at all. This
    // line stands in for that startup step.
    (void)SFT::Reflection::TypeRegistry::instance().type<Widget>();

    // ── Type/field/method/event lookup ────────────────────────────────────────────────────────
    SturdyReflectionId type = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_find_type("test.ffi.reflection.widget", &type) == STURDY_OK,
          "find_type must find the macro-reflected Widget");
    check(sturdy_reflection_find_type("does.not.exist", &type) != STURDY_OK,
          "find_type must fail cleanly for an unknown name, not crash");

    check(sturdy_reflection_find_type("test.ffi.reflection.widget", &type) == STURDY_OK, "re-resolving type must succeed");

    char name_buffer[128] = {};
    size_t name_length = 0;
    check(sturdy_reflection_type_name(type, name_buffer, sizeof(name_buffer), &name_length) == STURDY_OK,
          "type_name must succeed");
    check(std::strcmp(name_buffer, "test.ffi.reflection.widget") == 0, "type_name must round-trip the canonical name");

    uint32_t type_size = 0;
    uint32_t type_align = 0;
    check(sturdy_reflection_type_size(type, &type_size, &type_align) == STURDY_OK, "type_size must succeed");
    check(type_size == sizeof(Widget), "type_size must report the real C++ size");

    SturdyReflectionId health_field = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_find_field(type, "health", &health_field) == STURDY_OK, "find_field must find health");

    SturdyReflectionFieldInfo field_info{};
    check(sturdy_reflection_field_info(type, health_field, &field_info) == STURDY_OK, "field_info must succeed");
    check(field_info.size == sizeof(int), "field_info must report health's real size");
    check(field_info.is_static == STURDY_FALSE, "health must not be reported static");

    SturdyReflectionId take_damage_method = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_find_method(type, "take_damage", &take_damage_method) == STURDY_OK,
          "find_method must find take_damage");

    SturdyReflectionMethodInfo method_info{};
    check(sturdy_reflection_method_info(type, take_damage_method, &method_info) == STURDY_OK, "method_info must succeed");
    check(method_info.param_count == 1, "method_info must report take_damage's one parameter");

    SturdyReflectionId on_damaged_event = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_find_event(type, "on_damaged", &on_damaged_event) == STURDY_OK,
          "find_event must find on_damaged");

    // ── Field get/set ──────────────────────────────────────────────────────────────────────────
    Widget widget{};
    widget.health = 42;
    int read_health = 0;
    check(sturdy_reflection_get_field(type, &widget, health_field, &read_health, sizeof(read_health)) == STURDY_OK,
          "get_field must succeed");
    check(read_health == 42, "get_field must reflect the live value");

    const int new_health = 7;
    check(sturdy_reflection_set_field(type, &widget, health_field, &new_health, sizeof(new_health)) == STURDY_OK,
          "set_field must succeed");
    check(widget.health == 7, "set_field must mutate the live object");

    // ── Method invocation, then override, then before-hook ───────────────────────────────────────
    widget.health = 100;
    int amount = 30;
    const void *args[]{&amount};
    int result = 0;
    check(sturdy_reflection_invoke_method(type, &widget, take_damage_method, args, 1, &result, sizeof(result)) == STURDY_OK,
          "invoke_method must succeed");
    check(widget.health == 70 && result == 70, "invoke_method must run the real implementation");

    int override_calls = 0;
    check(sturdy_reflection_register_override(type, take_damage_method, &override_take_damage, &override_calls) == STURDY_OK,
          "register_override must succeed");
    widget.health = 100;
    result = 0;
    check(sturdy_reflection_invoke_method(type, &widget, take_damage_method, args, 1, &result, sizeof(result)) == STURDY_OK,
          "invoke_method with an override installed must succeed");
    check(widget.health == 40 && override_calls == 1, "the override must run instead of the real implementation");
    check(sturdy_reflection_clear_override(type, take_damage_method) == STURDY_OK, "clear_override must succeed");

    int hook_calls = 0;
    SturdyReflectionSubscription hook_subscription{};
    check(sturdy_reflection_add_before_hook(type, take_damage_method, &before_hook, &hook_calls, &hook_subscription) == STURDY_OK,
          "add_before_hook must succeed");
    widget.health = 100;
    check(sturdy_reflection_invoke_method(type, &widget, take_damage_method, args, 1, &result, sizeof(result)) == STURDY_OK,
          "invoke_method with a before-hook installed must succeed");
    check(widget.health == 70 && hook_calls == 1,
          "a before-hook must run alongside (not instead of) the real implementation");
    check(sturdy_reflection_remove_before_hook(type, take_damage_method, hook_subscription) == STURDY_OK,
          "remove_before_hook must succeed");

    // ── Events ─────────────────────────────────────────────────────────────────────────────────
    int received_amount = -1;
    SturdyReflectionSubscription event_subscription{};
    check(sturdy_reflection_subscribe_event(type, on_damaged_event, &event_listener, &received_amount, &event_subscription) ==
              STURDY_OK,
          "subscribe_event must succeed");
    int fired_amount = 55;
    const void *event_args[]{&fired_amount};
    check(sturdy_reflection_fire_event(type, &widget, on_damaged_event, event_args, 1) == STURDY_OK, "fire_event must succeed");
    check(received_amount == 55, "a subscribed listener must receive the fired argument");
    check(sturdy_reflection_unsubscribe_event(type, on_damaged_event, event_subscription) == STURDY_OK,
          "unsubscribe_event must succeed");

    // ── Dynamic type registration: no C++ type behind it at all ──────────────────────────────────
    SturdyReflectionTypeBuilder builder{};
    check(sturdy_reflection_type_builder_create("test.ffi.reflection.dynamic_item", sizeof(int), alignof(int), &builder) ==
              STURDY_OK,
          "type_builder_create must succeed");
    check(sturdy_reflection_type_builder_set_constructors(builder, &dynamic_move_construct, &dynamic_destroy,
                                                          &dynamic_default_construct, nullptr, nullptr) == STURDY_OK,
          "type_builder_set_constructors must succeed");
    check(sturdy_reflection_type_builder_add_field(builder, "durability", 0, sizeof(int), alignof(int),
                                                   STURDY_REFLECTION_ID_NONE, STURDY_TRUE, &dynamic_field_get,
                                                   &dynamic_field_set) == STURDY_OK,
          "type_builder_add_field must succeed");

    SturdyReflectionId dynamic_type = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_type_builder_finish(builder, &dynamic_type) == STURDY_OK, "type_builder_finish must succeed");

    SturdyReflectionId rediscovered_type = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_find_type("test.ffi.reflection.dynamic_item", &rediscovered_type) == STURDY_OK,
          "a dynamically-built type must be findable by name afterwards, exactly like a macro-reflected one");
    check(rediscovered_type.high == dynamic_type.high && rediscovered_type.low == dynamic_type.low,
          "find_type must return the same id type_builder_finish did");

    SturdyReflectionId durability_field = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_find_field(dynamic_type, "durability", &durability_field) == STURDY_OK,
          "a dynamically-built field must be found by name");

    int durability_storage = 5;
    int read_durability = 0;
    check(sturdy_reflection_get_field(dynamic_type, &durability_storage, durability_field, &read_durability,
                                      sizeof(read_durability)) == STURDY_OK,
          "get_field on a dynamically-built type must succeed");
    check(read_durability == 5, "get_field on a dynamically-built type must reflect the live value");

    check(sturdy_reflection_unregister_type(dynamic_type) == STURDY_OK, "unregister_type must succeed");
    check(sturdy_reflection_find_type("test.ffi.reflection.dynamic_item", &rediscovered_type) != STURDY_OK,
          "an unregistered dynamically-built type must no longer be findable");

    // A discarded (never-finished) builder must not register anything and must not crash.
    SturdyReflectionTypeBuilder discarded_builder{};
    check(sturdy_reflection_type_builder_create("test.ffi.reflection.never_finished", sizeof(int), alignof(int),
                                                &discarded_builder) == STURDY_OK,
          "creating a builder that will be discarded must still succeed");
    check(sturdy_reflection_type_builder_discard(discarded_builder) == STURDY_OK, "type_builder_discard must succeed");
    SturdyReflectionId never_finished = STURDY_REFLECTION_ID_NONE;
    check(sturdy_reflection_find_type("test.ffi.reflection.never_finished", &never_finished) != STURDY_OK,
          "a discarded builder must never have registered anything");

    if (failures != 0) {
        (void)std::fprintf(stderr, "ReflectionFfiTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
