/// Coverage for the compile-time-first binary serializer (`StaticSerialize.hpp`):
/// `static_serialize`/`static_deserialize` round-tripping a plain trivially-copyable type and a
/// type with a nested reflected field, including a genuine constant-expression (not merely
/// `constexpr`-markable) round trip.
///
/// Deliberately does NOT declare a type with a `UString`/`std::vector`/`std::optional`/smart-
/// pointer field: `static_serialize`/`static_deserialize` hard-`static_assert` on those shapes
/// (they belong to the runtime `serialize_to_bytes`/`to_document` path instead — see
/// `StaticSerialize.hpp`'s file-level doc comment), so writing one here would fail to compile.

#include <Reflection/Reflection.hpp>
#include <Reflection/StaticSerialize.hpp>

#include <cstddef>
#include <vector>

namespace {

    struct Position {
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
    };

    struct Player {
        int health = 100;
        Position position{};
    };

} // namespace

SFT_REFLECT_TYPE(Position, "test.reflection.static_serialize.position");
SFT_REFLECT_FIELD(x);
SFT_REFLECT_FIELD(y);
SFT_REFLECT_FIELD(z);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Player, "test.reflection.static_serialize.player");
SFT_REFLECT_FIELD(health);
SFT_REFLECT_FIELD(position);
SFT_REFLECT_END();

using namespace SFT::Reflection;

// ── Genuine constant-expression round trip: Position (flat, trivially copyable fields only) ────
constexpr bool position_roundtrip_ok = [] {
    Position original{1.5F, -2.5F, 3.25F};
    std::vector<std::byte> bytes;
    static_serialize(original, bytes);
    if (bytes.size() != sizeof(float) * 3) {
        return false;
    }
    Position restored{};
    const usize consumed = static_deserialize(restored, std::span<const std::byte>{bytes});
    if (consumed != bytes.size()) {
        return false;
    }
    return restored.x == original.x && restored.y == original.y && restored.z == original.z;
}();
static_assert(position_roundtrip_ok);

// ── Genuine constant-expression round trip: Player (nested reflected field) ────────────────────
constexpr bool player_roundtrip_ok = [] {
    Player original{};
    original.health = 42;
    original.position = Position{10.0F, 20.0F, 30.0F};

    std::vector<std::byte> bytes;
    static_serialize(original, bytes);
    if (bytes.size() != sizeof(int) + sizeof(float) * 3) {
        return false;
    }

    Player restored{};
    const usize consumed = static_deserialize(restored, std::span<const std::byte>{bytes});
    if (consumed != bytes.size()) {
        return false;
    }
    return restored.health == original.health && restored.position.x == original.position.x &&
           restored.position.y == original.position.y && restored.position.z == original.position.z;
}();
static_assert(player_roundtrip_ok);

int main() {
    // Runtime smoke test mirroring the static_assert above, so a debug build without full
    // constant-evaluation of every branch still exercises the same code paths at runtime.
    Player original{};
    original.health = 7;
    original.position = Position{1.0F, 2.0F, 3.0F};

    std::vector<std::byte> bytes;
    static_serialize(original, bytes);
    if (bytes.size() != sizeof(int) + sizeof(float) * 3) {
        return 1;
    }

    Player restored{};
    const usize consumed = static_deserialize(restored, std::span<const std::byte>{bytes});
    if (consumed != bytes.size()) {
        return 1;
    }
    if (restored.health != original.health) {
        return 1;
    }
    if (restored.position.x != original.position.x || restored.position.y != original.position.y ||
        restored.position.z != original.position.z) {
        return 1;
    }
    return 0;
}
