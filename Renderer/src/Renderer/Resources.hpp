#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

#include <glm/vec3.hpp>

#include <RHI/RHI.hpp>
#include "Handles.hpp"
#include "Geometry.hpp"

using std::string;
using std::vector;

namespace SFT::Renderer {

    // A mesh's data no longer owns a dedicated GPU buffer — it's a sub-range of the Renderer's shared
    // vertex/index arenas (see Renderer::vertex_arena_/index_arena_ and try_upload_mesh), so any number
    // of distinct meshes can be drawn from one bound buffer via per-draw base_vertex/first_index —
    // the prerequisite for indirect/multi-draw across heterogeneous geometry.
    struct MeshResource {
        MeshHandle handle{};
        string label;
        vector<GeometryVertex> vertices;
        vector<u32> indices;
        // Element (not byte) offsets into the shared arenas — directly usable as
        // DrawIndexedArgs::base_vertex / DrawIndexedArgs::first_index / DrawArgs::first_vertex.
        u32 vertex_offset = 0;
        u32 index_offset = 0;
        bool gpu_resident = false;
        bool alive = false;
        // Object-space bounding sphere (mesh-local, before any world_transform), computed once from
        // `vertices` at upload time — CPU frustum culling (Culling.hpp, applied per geometry pass in
        // RendererLifecycle.cpp) transforms this by each RenderItem's world_transform rather than
        // recomputing it from raw geometry every frame.
        glm::vec3 bounds_center{0.0f};
        f32 bounds_radius = 0.0f;
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
