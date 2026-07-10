module;

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

export module Sturdy.Renderer:Resources;

import Sturdy.Foundation;
import Sturdy.RHI;
import :Handles;
import :Geometry;

using std::string;
using std::vector;

export namespace SFT::Renderer {

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
    };

} // namespace SFT::Renderer
