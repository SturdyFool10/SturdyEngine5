/// Coverage for `StaticDebugFormat.hpp`'s generated-per-type debug formatter — the first
/// consumer of the compile-time reflection layer that generates a new piece of code (a whole
/// formatting function) per reflected type, rather than merely computing a value from one (see
/// that file's doc comment, and `plans/reflection-hybrid-vision.md`'s metaprogramming/codegen
/// section for why this exists).

#include <Reflection/Reflection.hpp>
#include <Reflection/StaticDebugFormat.hpp>

#include <cstdio>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "StaticDebugFormatTest: %s\n", description);
            ++failures;
        }
    }

    struct Position {
        float x = 0.0F;
        float y = 0.0F;
    };

    struct Player {
        int health = 100;
        bool alive = true;
        UString name;
        Position position;
    };

} // namespace

SFT_REFLECT_TYPE(Position, "test.reflection.debug_format.position");
SFT_REFLECT_FIELD(x);
SFT_REFLECT_FIELD(y);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Player, "test.reflection.debug_format.player");
SFT_REFLECT_FIELD(health);
SFT_REFLECT_FIELD(alive);
SFT_REFLECT_FIELD(name);
SFT_REFLECT_FIELD(position);
SFT_REFLECT_END();

int main() {
    using namespace SFT::Reflection;

    const Position position{.x = 1.5F, .y = -2.0F};
    check(static_debug_string(position) == "test.reflection.debug_format.position{x: 1.500000, y: -2.000000}",
          "static_debug_string must render a flat reflected struct's fields in declaration order");

    Player player{};
    player.health = 42;
    player.alive = false;
    player.name = UString{"Aria"};
    player.position = position;
    const std::string rendered = static_debug_string(player);
    check(rendered == "test.reflection.debug_format.player{health: 42, alive: false, name: \"Aria\", "
                       "position: test.reflection.debug_format.position{x: 1.500000, y: -2.000000}}",
          "static_debug_string must recurse into a nested reflected struct field and quote UString fields");

    return failures == 0 ? 0 : 1;
}
