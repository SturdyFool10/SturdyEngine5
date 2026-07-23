#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

namespace SFT::Renderer {

    // Descriptor-free, renderer-level vertex shape for simple geometry submission. More specialized
    // vertex layouts can still use the lower-level RHI escape hatch directly.
    struct GeometryVertex {
        glm::vec3 position{};
        glm::vec3 normal{};
        glm::vec2 uv{};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        // xyz = tangent direction, w = bitangent handedness sign (+1/-1) — matches glTF's TANGENT
        // accessor convention exactly, so glTF-imported vertices map onto this field directly. Only
        // meaningful once a mesh source actually populates it (glTF's TANGENT attribute, or generated
        // per-triangle when a primitive omits it — see GltfImport.cpp); procedural Mesh primitives
        // (uv_sphere/cube/...) leave it at this default and don't support normal mapping yet.
        glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    };

} // namespace SFT::Renderer
