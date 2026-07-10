module;

#pragma region Imports
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#pragma endregion

export module Sturdy.Renderer:Geometry;

export namespace SFT::Renderer {

    // Descriptor-free, renderer-level vertex shape for simple geometry submission. More specialized
    // vertex layouts can still use the lower-level RHI escape hatch directly.
    struct GeometryVertex {
        glm::vec3 position{};
        glm::vec3 normal{};
        glm::vec2 uv{};
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    };

} // namespace SFT::Renderer
