/// Proves that building with `STURDY_REFLECTION_ENABLE_MODDING=0` (set via this test's own
/// `target_compile_definitions`, see `Reflection/CMakeLists.txt`) really does turn
/// `SFT_REFLECT_INVOKE`/`SFT_REFLECT_FIRE_EVENT` into a plain direct call/no-op — not just
/// "override always null" — by never once touching `TypeRegistry`: this test does not call
/// `TypeRegistry::instance().type<T>()` anywhere, yet `SFT_REFLECT_INVOKE` still produces the
/// correct result, because in this configuration it does not need a `TypeInfo` at all.

#include <cstdio>

#include <Reflection/Reflection.hpp>

#if STURDY_REFLECTION_ENABLE_MODDING
#error "This test exists specifically to exercise the STURDY_REFLECTION_ENABLE_MODDING=0 path."
#endif

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "ModdingDisabledTest: %s\n", description);
            ++failures;
        }
    }

    struct PlayerController {
        int health = 100;

        int take_damage(int amount) noexcept {
            health -= amount;
            return health;
        }
    };

} // namespace

SFT_REFLECT_TYPE(PlayerController, "test.reflection.modding_disabled.player_controller");
SFT_REFLECT_METHOD(take_damage);
SFT_REFLECT_EVENT("on_damaged", int);
SFT_REFLECT_END();

int main() {
    PlayerController player{};

    // No `TypeRegistry::instance().type<PlayerController>()` call anywhere above or below: with
    // modding compiled out, SFT_REFLECT_INVOKE must not need one.
    const int result = SFT_REFLECT_INVOKE(&player, PlayerController, take_damage, 30);
    check(result == 70, "SFT_REFLECT_INVOKE must still produce the direct call's result");
    check(player.health == 70, "SFT_REFLECT_INVOKE must still mutate the object");

    // The event macro must still evaluate its arguments (for side effects) even though it does
    // nothing else.
    int side_effect_calls = 0;
    auto make_arg = [&side_effect_calls]() noexcept -> int {
        ++side_effect_calls;
        return 30;
    };
    SFT_REFLECT_FIRE_EVENT(&player, PlayerController, "on_damaged", make_arg());
    check(side_effect_calls == 1, "SFT_REFLECT_FIRE_EVENT must still evaluate its arguments exactly once");

    if (failures != 0) {
        (void)std::fprintf(stderr, "ModdingDisabledTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
