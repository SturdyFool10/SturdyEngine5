#pragma once

// Plain-data types shared by several CxxApi subsystems. Like every header under CxxApi/Types/, this
// includes only <cstddef>/<cstdint> so bindgen can parse it on its own (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// A 3-component value that may be absent (`ok == false`), e.g. a projection behind the camera.
    struct Vec3Result {
        bool ok = false;
        float value[3] = {0.0f, 0.0f, 0.0f};
    };

    /// A ray in world space.
    struct Ray {
        float origin[3] = {0.0f, 0.0f, 0.0f};
        float direction[3] = {0.0f, 0.0f, 1.0f};
    };

    /// A ray that may be absent (`ok == false`).
    struct RayResult {
        bool ok = false;
        Ray ray{};
    };

    /// An axis-aligned box.
    struct Aabb {
        float min[3] = {0.0f, 0.0f, 0.0f};
        float max[3] = {0.0f, 0.0f, 0.0f};
    };

} // namespace SFT::CxxApi
