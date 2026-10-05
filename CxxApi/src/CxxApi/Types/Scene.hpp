#pragma once

// Plain-data types for entities and their placement. bindgen-safe: <cstddef>/<cstdint> and other
// Types/ headers only (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// Mirrors `Ecs::Entity`. `generation == 0` is "no entity".
    struct Entity {
        std::uint32_t index = 0;
        std::uint32_t generation = 0;
    };

    /// Mirrors `Engine::Transform`: placement relative to the parent (or the world without one).
    struct TransformDesc {
        float translation[3] = {0.0f, 0.0f, 0.0f};
        /// xyzw quaternion.
        float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float scale[3] = {1.0f, 1.0f, 1.0f};
    };

    /// Mirrors `Engine::PropagationStats`.
    struct TransformPropagationStats {
        std::uint32_t roots = 0;
        std::uint32_t children = 0;
        std::uint32_t orphans = 0;
        std::uint32_t cycles = 0;
    };

} // namespace SFT::CxxApi
