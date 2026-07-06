module;

#pragma region Imports
#include <glm/vec3.hpp>
#pragma endregion

export module Sturdy.Core:Triangle;

export namespace SFT::Core {

    // A single triangle in 3D space — three points, nothing else. The basis primitive for the
    // engine's geometry: anything built from triangles (meshes, collision shapes, etc.) is expected
    // to bottom out here rather than each defining its own three-point shape.
    struct Triangle {
        glm::vec3 a;
        glm::vec3 b;
        glm::vec3 c;
    };

} // namespace SFT::Core
