/// Coverage for compile-time properties (`StaticProperty.hpp`): a getter/setter pair exposed as a
/// single reflected value while the backing field stays private. Like `StaticReflectionTest.cpp`,
/// none of this touches `TypeRegistry` — every check below either is a `static_assert` (proving
/// it is genuinely `consteval`/`constexpr`) or operates on a plain stack object with no
/// reflection registration performed anywhere in this file.

#include <Reflection/StaticProperty.hpp>
#include <Reflection/StaticTypeId.hpp>

namespace {

    /// `health_` is never reflected, never public — `Player.health` is only ever reachable
    /// through the `health_property` property below, exactly the "editors/mods/scripting see
    /// `Player.health` without it being a raw public data member" case this header exists for.
    class Player {
      public:
        [[nodiscard]] constexpr int get_health() const noexcept {
            return health_;
        }

        constexpr void set_health(int value) noexcept {
            health_ = value;
        }

      private:
        int health_ = 100;
    };

} // namespace

using namespace SFT::Reflection;

// ── Property type identity: derived from the getter's return type ─────────────────────────────
constexpr auto health_property = property<Player, &Player::get_health, &Player::set_health, "health">();
static_assert(health_property.type() == type_id<int>());
static_assert(health_property.name() == "health");

// A read-only property (no setter supplied) is still fully usable for reading.
constexpr auto readonly_health_property = property<Player, &Player::get_health>();
static_assert(readonly_health_property.type() == type_id<int>());

// ── has_setter(): a compile-time-only property of the (Getter, Setter) pair itself ────────────
static_assert(health_property.has_setter());
static_assert(!readonly_health_property.has_setter());

// ── Genuine compile-time round trip: construct, write through the property, read it back ──────
constexpr int property_round_trip = [] {
    Player player{};
    health_property.set(player, 250);
    return health_property.get(player);
}();
static_assert(property_round_trip == 250);

int main() {
    // Runtime smoke test mirroring the static_assert above, so a debug build without full
    // constant-evaluation of every branch still exercises the same code paths at runtime.
    Player player{};
    if (health_property.get(player) != 100) {
        return 1;
    }
    health_property.set(player, 50);
    if (player.get_health() != 50) {
        return 1;
    }
    if (health_property.get(player) != 50) {
        return 1;
    }
    if (readonly_health_property.get(player) != 50) {
        return 1;
    }
    return 0;
}
