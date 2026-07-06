module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <array>
#include <cstddef>
#pragma endregion

export module Sturdy.Core:VulkanVertex;

import Sturdy.Foundation;

export namespace SFT::Core::Vulkan {

    // Plain interleaved per-vertex layout for the demo pipeline: a 2D position (already in
    // clip-space-ready [-1, 1] coordinates, no MVP transform yet) plus an RGB color, matching
    // Shaders/triangle.slang's VertexInput field-for-field (position at location 0, color at
    // location 1 — see vertex_attribute_descriptions() below).
    struct Vertex {
        float position[2];
        float color[3];
    };

    [[nodiscard]] constexpr VkVertexInputBindingDescription vertex_binding_description() noexcept {
        return VkVertexInputBindingDescription{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
    }

    [[nodiscard]] constexpr std::array<VkVertexInputAttributeDescription, 2> vertex_attribute_descriptions() noexcept {
        return {
            VkVertexInputAttributeDescription{
                .location = 0,
                .binding = 0,
                .format = VK_FORMAT_R32G32_SFLOAT,
                .offset = offsetof(Vertex, position),
            },
            VkVertexInputAttributeDescription{
                .location = 1,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, color),
            },
        };
    }

    // A regular hexagon (circumradius 0.5, centered on the origin): 7 unique vertices — a white
    // center (index 0) plus 6 rim points (indices 1-6, one per pure color evenly spaced around the
    // color wheel) — drawn as 6 triangles via hexagon_indices() below, rather than repeating shared
    // vertices per triangle the way the first (vertex-buffer-only) version of this demo did.
    [[nodiscard]] constexpr std::array<Vertex, 7> hexagon_vertices() noexcept {
        return {
            Vertex{{0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},          // 0: center, white
            Vertex{{0.5f, 0.0f}, {1.0f, 0.0f, 0.0f}},          // 1: 0 deg, red
            Vertex{{0.25f, 0.4330127f}, {1.0f, 1.0f, 0.0f}},   // 2: 60 deg, yellow
            Vertex{{-0.25f, 0.4330127f}, {0.0f, 1.0f, 0.0f}},  // 3: 120 deg, green
            Vertex{{-0.5f, 0.0f}, {0.0f, 1.0f, 1.0f}},         // 4: 180 deg, cyan
            Vertex{{-0.25f, -0.4330127f}, {0.0f, 0.0f, 1.0f}}, // 5: 240 deg, blue
            Vertex{{0.25f, -0.4330127f}, {1.0f, 0.0f, 1.0f}},  // 6: 300 deg, magenta
        };
    }

    // The 6 triangles fanning hexagon_vertices() out from its center (index 0) into a filled
    // hexagon. `u32` here (rather than `u16`) even though this one tiny mesh obviously fits in 16
    // bits — a real mesh can easily clear 65535 vertices (a single modern-game mesh can be tens of
    // thousands of triangles), so 32-bit indices are the right default; nothing here forces every
    // future mesh to the same index type regardless, since VulkanCommandBuffer::bind_index_buffer()
    // takes the VkIndexType to use explicitly.
    //
    // Winding: each triangle lists its two rim vertices in *decreasing* angle order
    // (center, P[i+1], P[i]) to match the same front-face winding Shaders/triangle.slang's original
    // hardcoded triangle used — this pipeline's rasterization state (see VulkanBackendPipeline.cpp)
    // culls back faces with frontFace = COUNTER_CLOCKWISE.
    [[nodiscard]] constexpr std::array<u32, 18> hexagon_indices() noexcept {
        return {
            0, 2, 1,
            0, 3, 2,
            0, 4, 3,
            0, 5, 4,
            0, 6, 5,
            0, 1, 6,
        };
    }

} // namespace SFT::Core::Vulkan
