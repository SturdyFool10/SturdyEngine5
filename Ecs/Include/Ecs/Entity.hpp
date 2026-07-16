#pragma once

#include <Foundation/Foundation.hpp>

namespace SFT::Ecs {

    // A small, stable handle identifying one live object. `index` is a slot in World's entity-record
    // table; `generation` is bumped every time that slot is freed and reused, so a stale copy of an
    // Entity taken before a destroy() compares unequal to whatever now occupies the same index —
    // World::is_alive() is the O(1) check. `generation == 0` is reserved for "no entity" (a
    // default-constructed Entity is never a live one, since World hands out generation 1 on first use
    // of a slot).
    struct Entity {
        u32 index = 0;
        u32 generation = 0;

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return generation != 0; }
        friend constexpr bool operator==(Entity, Entity) noexcept = default;
    };

} // namespace SFT::Ecs
