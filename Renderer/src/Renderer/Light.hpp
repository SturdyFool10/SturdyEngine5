#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec3.hpp>
#pragma endregion

namespace SFT::Renderer {

    // The scene's single sun/moon light. `direction` points FROM the light TOWARD the scene—the
    // direction its emitted light travels.
    struct DirectionalLight {
        glm::vec3 direction{0.35f, -0.75f, 0.55f};
        glm::vec3 radiance{4.0f, 3.75f, 3.35f};
        // Half-angle of the emitter in degrees. The real sun is about 0.27 degrees; PCSS uses this
        // to grow the penumbra with receiver/blocker separation while retaining contact-hard edges.
        f32 angular_radius_degrees = 0.27f;
        bool casts_shadows = true;
    };

    // A cone-shaped local light. `inner_cone_cos`/`outer_cone_cos` are cosines of the half-angles
    // where falloff starts/ends (glTF punctual-light convention) rather than raw angles, so shaders
    // can use them directly without a per-pixel acos/cos.
    struct SpotLight {
        glm::vec3 position{0.0f};
        glm::vec3 direction{0.0f, -1.0f, 0.0f};
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 inner_cone_cos = 0.97f;
        f32 outer_cone_cos = 0.90f;
        // Radius of the disk emitter in world units. Zero still receives a small antialiasing PCF
        // kernel, while positive values enable contact-hardening soft shadows.
        f32 source_radius = 0.05f;
        bool casts_shadows = true;
    };

    // An omnidirectional local light.
    struct PointLight {
        glm::vec3 position{0.0f};
        glm::vec3 radiance{1.0f};
        f32 range = 10.0f;
        f32 source_radius = 0.05f;
        bool casts_shadows = true;
    };

} // namespace SFT::Renderer
