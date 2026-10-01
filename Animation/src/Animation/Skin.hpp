#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <span>
#include <vector>

namespace SFT::Animation {

    /// Up to four joint influences per vertex (glTF JOINTS_0 / WEIGHTS_0). Joint indices address the
    /// skeleton the weights were authored against.
    struct SkinWeights {
        std::vector<glm::uvec4> joints;
        std::vector<glm::vec4> weights;

        [[nodiscard]] usize vertex_count() const noexcept { return weights.size(); }
    };

    struct SkinnedVertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec3 tangent;
    };

    /// Linear blend skinning of one vertex by `skin_matrices` (joint model matrix times inverse bind).
    [[nodiscard]] inline SkinnedVertex skin_vertex(std::span<const glm::mat4> skin_matrices, const glm::uvec4 &joints,
                                                    const glm::vec4 &weights, const glm::vec3 &position,
                                                    const glm::vec3 &normal, const glm::vec3 &tangent) noexcept {
        glm::mat4 blended(0.0f);
        f32 total = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (weights[i] > 0.0f && joints[i] < skin_matrices.size()) {
                blended += skin_matrices[joints[i]] * weights[i];
                total += weights[i];
            }
        }
        if (total <= 0.0f) {
            return {position, normal, tangent};
        }
        blended /= total;
        const glm::mat3 linear(blended);
        const auto safe_normalize = [](const glm::vec3 &v, const glm::vec3 &fallback) {
            const f32 len = glm::length(v);
            return len > 1e-8f ? v / len : fallback;
        };
        return {
            glm::vec3(blended * glm::vec4(position, 1.0f)),
            safe_normalize(linear * normal, normal),
            safe_normalize(linear * tangent, tangent),
        };
    }

} // namespace SFT::Animation
