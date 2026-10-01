#pragma once

#include <Foundation/Foundation.hpp>

#include <span>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace SFT::Renderer {

    /// Up to four joint influences for one vertex. Joint indices address the matrix array given to
    /// `Renderer::set_skin_pose`.
    struct SkinInfluence {
        glm::uvec4 joints{0};
        glm::vec4 weights{0.0f};
    };
    static_assert(sizeof(SkinInfluence) == 32);

    /// One vertex's offset for one morph target; layout matches the shader (`Animation::MorphEntry`).
    struct MorphDelta {
        glm::vec3 position_delta{0.0f};
        u32 target = 0;
        glm::vec3 normal_delta{0.0f};
        u32 padding = 0;
    };
    static_assert(sizeof(MorphDelta) == 32);

    /// Sparse per-vertex morph data (CSR): vertex `v` owns `entries[vertex_offsets[v] .. vertex_offsets[v + 1])`.
    struct SkinMorphDesc {
        u32 target_count = 0;
        std::span<const u32> vertex_offsets; // vertex_count + 1 entries
        std::span<const MorphDelta> entries;
    };

    struct SkinAttachDesc {
        /// One entry per vertex of the mesh.
        std::span<const SkinInfluence> influences;
        /// The mesh's culling sphere is computed from its bind pose; a posed character leaves it, so the radius is
        /// multiplied by this to keep the mesh from being culled while its limbs are outside the bind-pose sphere.
        f32 bounds_scale = 2.0f;
        /// Optional blend shapes, applied in bind space before the joints (glTF order).
        SkinMorphDesc morph;
    };

} // namespace SFT::Renderer
