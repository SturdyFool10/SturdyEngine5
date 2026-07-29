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
        // Immutable draw metadata must remain resident even when the optional CPU recovery payload
        // below is released. In particular, index_count determines indexed vs. non-indexed draws.
        u32 vertex_count = 0;
        u32 index_count = 0;
        bool gpu_resident = false;
        bool alive = false;
        // False (the default) means `vertices`/`indices` above get their capacity released right
        // after the initial upload succeeds (Renderer::create_mesh); vertex_count/index_count remain
        // available for drawing. True keeps the recovery payload populated
        // forever so Renderer::try_upload_mesh can replay this mesh's upload after a Vulkan
        // device-loss event (see restore_gpu_resources_after_recovery); a mesh created with this
        // false and later hit by device loss simply can't be recovered — try_upload_mesh detects the
        // empty arrays and skips it rather than uploading garbage or aborting recovery for every
        // other mesh. Set from Engine::AssetManager::ModelAssetDesc::retain_cpu_mesh_data.
        bool retain_cpu_copy = false;
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
        // False for borrowed views whose wrapper lifetime belongs to another Renderer resource (an
        // off-screen target). Public destroy_texture() must not invalidate such a target-owned wrapper;
        // destroying the owning target retires it explicitly.
        bool externally_destroyable = true;
    };

} // namespace SFT::Renderer
