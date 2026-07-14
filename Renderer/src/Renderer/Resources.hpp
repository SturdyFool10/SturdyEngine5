#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include "Handles.hpp"
#include "Geometry.hpp"

using std::string;
using std::vector;

namespace SFT::Renderer {

    struct MeshResource {
        MeshHandle handle{};
        string label;
        vector<GeometryVertex> vertices;
        vector<u32> indices;
        RHI::BufferHandle vertex_buffer{};
        RHI::BufferHandle index_buffer{};
        bool gpu_resident = false;
        bool alive = false;

        [[nodiscard]] RHI::BufferHandle vertex_buffer_handle() const noexcept { return vertex_buffer; }
        [[nodiscard]] RHI::BufferHandle index_buffer_handle() const noexcept { return index_buffer; }
    };

    struct MaterialResource {
        MaterialHandle handle{};
        string label;
        bool alive = false;
    };

    struct TextureResource {
        TextureHandle handle{};
        string label;
        RHI::TextureHandle texture{};
        RHI::TextureViewHandle view{};
        RHI::SamplerHandle sampler{};
        bool alive = false;
        // False for a handle minted by Renderer::adopt_texture() — the caller created (and keeps
        // owning) `texture`/`view`/`sampler`, so destroy_texture() must release only this wrapper
        // entry, not the underlying RHI objects.
        bool owns_gpu_resources = true;
    };

} // namespace SFT::Renderer
