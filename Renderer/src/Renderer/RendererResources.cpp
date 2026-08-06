#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <expected>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;

namespace SFT::Renderer {

    Core::RendererExpected<MeshHandle> Renderer::create_mesh(span<const GeometryVertex> vertices,
                                                             span<const u32> indices,
                                                             const char *label) {
        ZoneScopedN("Renderer::create_mesh");
        if (vertices.empty()) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Cannot create a mesh with no vertices."});
        }
        if (vertices.size() > std::numeric_limits<u32>::max() || indices.size() > std::numeric_limits<u32>::max()) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Mesh vertex/index count exceeds the renderer's 32-bit draw limits."});
        }

        MeshResource mesh{};
        mesh.handle = MeshHandle{static_cast<u64>(meshes_.size() + 1)};
        mesh.label = label ? label : "";
        mesh.vertices.assign(vertices.begin(), vertices.end());
        mesh.indices.assign(indices.begin(), indices.end());
        mesh.vertex_count = static_cast<u32>(vertices.size());
        mesh.index_count = static_cast<u32>(indices.size());
        mesh.alive = true;

        if (Core::RendererResult upload = try_upload_mesh(mesh); !upload.has_value()) {
            return unexpected(upload.error());
        }


        meshes_.push_back(std::move(mesh));
        return meshes_.back().handle;
    }

    Core::RendererExpected<MeshHandle> Renderer::upload(Mesh &mesh) {
        ZoneScopedN("Renderer::upload");
        if (mesh.is_gpu_resident()) {
            return mesh.gpu_handle();
        }

        Core::RendererExpected<MeshHandle> handle =
            create_mesh(mesh.vertices(), mesh.indices(), mesh.label().empty() ? nullptr : mesh.label().c_str());
        if (!handle.has_value()) {
            return unexpected(handle.error());
        }

        mesh.mark_uploaded(*handle);
        return *handle;
    }

    void Renderer::destroy_mesh(MeshHandle handle) noexcept {
        ZoneScopedN("Renderer::destroy_mesh");
        MeshResource *resource = mesh(handle);
        if (!resource) {
            return;
        }

        if (resource->bottom_level_acceleration_structure) {
            // A submitted frame's TLAS may still reference this BLAS. Mesh destruction is uncommon;
            // use the coarse safe point until BLAS retirement is tracked per frame slot.
            wait_idle();
            if (RHI::RhiDevice *device = rhi_device()) {
                device->destroy_acceleration_structure(resource->bottom_level_acceleration_structure);
            }
            resource->bottom_level_acceleration_structure = {};
        }

        // The mesh's bytes live in the shared vertex/index arenas (try_upload_mesh), not a dedicated
        // buffer this resource owns, so there's nothing to individually free here — only whole-arena
        // teardown (destroy_all_resources) or a real free-list allocator (not implemented yet; nothing
        // else in the engine reclaims evicted GPU memory either — see asset-manager.md) could reclaim
        // this range. Evicting a mesh here just stops it from being drawn/replayed on future growth.
        vector<GeometryVertex>{}.swap(resource->vertices);
        vector<u32>{}.swap(resource->indices);
        resource->vertex_offset = 0;
        resource->index_offset = 0;
        resource->vertex_count = 0;
        resource->index_count = 0;
        resource->gpu_resident = false;
        resource->alive = false;
    }

    MeshResource *Renderer::mesh(MeshHandle handle) noexcept {
        ZoneScopedN("Renderer::mesh");
        if (!handle || handle.value > meshes_.size()) {
            return nullptr;
        }
        MeshResource &resource = meshes_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    const MeshResource *Renderer::mesh(MeshHandle handle) const noexcept {
        ZoneScopedN("Renderer::mesh");
        if (!handle || handle.value > meshes_.size()) {
            return nullptr;
        }
        const MeshResource &resource = meshes_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    Core::RendererExpected<MaterialHandle> Renderer::create_material(const char *label) {
        ZoneScopedN("Renderer::create_material");
        MaterialResource material{};
        material.handle = MaterialHandle{static_cast<u64>(materials_.size() + 1)};
        material.label = label ? label : "";
        material.alive = true;
        materials_.push_back(std::move(material));
        return materials_.back().handle;
    }

    void Renderer::destroy_material(MaterialHandle handle) noexcept {
        ZoneScopedN("Renderer::destroy_material");
        MaterialResource *resource = material(handle);
        if (!resource) {
            return;
        }
        resource->label.clear();
        resource->alive = false;
    }

    MaterialResource *Renderer::material(MaterialHandle handle) noexcept {
        ZoneScopedN("Renderer::material");
        if (!handle || handle.value > materials_.size()) {
            return nullptr;
        }
        MaterialResource &resource = materials_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    const MaterialResource *Renderer::material(MaterialHandle handle) const noexcept {
        ZoneScopedN("Renderer::material");
        if (!handle || handle.value > materials_.size()) {
            return nullptr;
        }
        const MaterialResource &resource = materials_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    void Renderer::destroy_all_resources() noexcept {
        ZoneScopedN("Renderer::destroy_all_resources");
        if (shader_hot_reload_poll_) {
            (void)shader_hot_reload_poll_->wait();
            shader_hot_reload_poll_.reset();
        }
        shader_watcher_.reset();

        frame_draws_.clear();

        for (MaterialInstanceResource &resource : material_instances_) {
            if (resource.alive) {
                destroy_material_instance_gpu(resource);
                resource = {};
            }
        }
        material_instances_.clear();
        // Specialized material pipeline variants must go before the templates/modules they link.
        destroy_object_history_resources();
        destroy_instance_cull_resources();
        for (MaterialTemplateResource &resource : material_templates_) {
            if (resource.alive) {
                destroy_material_template_gpu(resource);
                resource = {};
            }
        }
        material_templates_.clear();

        destroy_deferred_msaa_resources();
        destroy_tonemap_resources();
        destroy_text_overlay_resources();

        for (MeshResource &resource : meshes_) {
            if (resource.alive) {
                destroy_mesh(resource.handle);
            }
        }
        if (RHI::RhiDevice *device = rhi_device()) {
            if (vertex_arena_.buffer) {
                device->destroy_buffer(vertex_arena_.buffer);
            }
            if (index_arena_.buffer) {
                device->destroy_buffer(index_arena_.buffer);
            }
        }
        vertex_arena_ = GeometryArena{.usage = vertex_arena_.usage};
        index_arena_ = GeometryArena{.usage = index_arena_.usage};
        for (MaterialResource &resource : materials_) {
            resource.alive = false;
            resource.label.clear();
        }
        // Off-screen targets own the GPU objects behind non-owning TextureHandle sampling wrappers.
        // Destroy those owners first; the texture table loop below then only destroys resources whose
        // wrapper itself owns the GPU allocation.
        destroy_all_offscreen_render_targets();
        for (TextureResource &resource : textures_) {
            if (resource.owns_gpu_resources) {
                if (RHI::RhiDevice *device = rhi_device()) {
                    if (resource.sampler) {
                        device->destroy_sampler(resource.sampler);
                    }
                    if (resource.view) {
                        device->destroy_texture_view(resource.view);
                    }
                    if (resource.texture) {
                        device->destroy_texture(resource.texture);
                    }
                }
            }
            resource = {};
        }
        textures_.clear();

        // Reclaim every window's in-flight frame slots and destroy each slot's (reusable) fence, then the
        // window's presentation resources. Safe: teardown runs after the destructor's wait_idle(), so no
        // slot's GPU work is still pending. Also normally a no-op for scene_frame_resources/hiz_pyramid
        // by this point — every window already ran destroy_window_surface() (which cleans up its own
        // record's copies) before ~Renderer() gets here — but kept as a defensive sweep over whatever
        // window_surfaces_ still holds, rather than assuming that's always true.
        auto window_surfaces_guard = window_surfaces_.lock();
        for (auto &record : *window_surfaces_guard) {
            if (RHI::RhiDevice *device = rhi_device()) {
                for (FrameInFlight &slot : record->frames_in_flight) {
                    reclaim_frame_slot(slot, true);
                    destroy_text_frame_resources(*device, slot.text_overlay_resources);
                    destroy_frame_bloom_targets(slot);
                    destroy_frame_composite_target(slot);
                    destroy_frame_shadow_targets(slot);
                    destroy_frame_atmosphere_targets(slot);
                    destroy_frame_deferred_targets(slot);
                    if (slot.fence) {
                        device->destroy_fence(slot.fence);
                    }
                }
            }
            record->frames_in_flight.clear();
            destroy_scene_gpu_resources(record->scene_frame_resources);
            destroy_hiz_pyramid(record->hiz_pyramid);
            destroy_rhi_presentation_resources(*record);
        }
        // Frame-slot bloom bind groups reference bloom_'s cached layout and sampler, so they must be
        // destroyed above before the shared pipeline resources are torn down.
        destroy_bloom_resources();
        destroy_bloom_composite_resources();
        destroy_shadow_lighting_resources();
        destroy_spectral_path_tracing_resources();
        destroy_custom_post_process_resources();
        destroy_custom_compute_effect_resources();
        destroy_atmosphere_lut_resources();
        destroy_hiz_build_resources();
    }

    Core::RendererResult Renderer::grow_geometry_arena(GeometryArena &arena, u64 required_bytes, const char *label) {
        ZoneScopedN("Renderer::grow_geometry_arena");
        RHI::RhiDevice *device = rhi_device();
        if (!device) {
            return {};
        }
        u64 new_capacity = std::max<u64>(required_bytes, std::max<u64>(arena.capacity_bytes * 2, 1u << 20));
        RHI::BufferDesc desc{
            .size = new_capacity,
            .usage = arena.usage,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = label,
        };
        auto new_buffer = device->create_buffer(desc);
        if (!new_buffer) {
            return unexpected(graphics_error_from_rhi(new_buffer.error(), "grow geometry arena"));
        }

        // Bulk GPU-to-GPU copy of the old buffer's already-written region into the new, bigger one —
        // arena offsets are append-only/stable across growth, so this is one copy, not a replay of
        // every mesh's retained CPU payload. Growth is a routine asset-loading operation, unlike the
        // full per-mesh replay used after backend reconstruction.
        if (arena.buffer && arena.used_bytes > 0) {
            auto encoder = device->create_command_encoder(RHI::CommandEncoderDesc{.label = "geometry arena growth copy"});
            if (!encoder) {
                device->destroy_buffer(*new_buffer);
                return unexpected(graphics_error_from_rhi(encoder.error(), "create geometry arena growth encoder"));
            }
            const RHI::BufferCopy region{.src_offset = 0, .dst_offset = 0, .size = arena.used_bytes};
            (*encoder)->copy_buffer_to_buffer(arena.buffer, *new_buffer, region);

            auto command_buffer = (*encoder)->finish();
            if (!command_buffer) {
                device->destroy_buffer(*new_buffer);
                return unexpected(graphics_error_from_rhi(command_buffer.error(), "finish geometry arena growth encoder"));
            }

            auto fence = device->create_fence(RHI::FenceDesc{.label = "geometry arena growth fence"});
            if (!fence) {
                device->destroy_command_buffer(*command_buffer);
                device->destroy_buffer(*new_buffer);
                return unexpected(graphics_error_from_rhi(fence.error(), "create geometry arena growth fence"));
            }

            const array command_buffers{*command_buffer};
            RHI::SubmitDesc submit_desc{
                .command_buffers = span<const RHI::CommandBufferHandle>{command_buffers.data(), command_buffers.size()},
                .fence = *fence,
                .flags = RHI::SubmitFlags::OneShot,
                .label = "geometry arena growth submit",
            };
            if (auto submitted = device->submit(submit_desc); !submitted) {
                device->destroy_fence(*fence);
                device->destroy_command_buffer(*command_buffer);
                device->destroy_buffer(*new_buffer);
                return unexpected(graphics_error_from_rhi(submitted.error(), "submit geometry arena growth"));
            }
            auto waited = device->wait_fences(span<const RHI::FenceHandle>{&*fence, 1}, true);
            if (!waited) {
                device->destroy_fence(*fence);
                device->destroy_command_buffer(*command_buffer);
                device->destroy_buffer(*new_buffer);
                return unexpected(graphics_error_from_rhi(waited.error(), "wait geometry arena growth fence"));
            }
            if (!*waited) {
                // Real timeout (this call uses wait_forever, so unreachable outside a device hang) --
                // the fence is not confirmed signaled, so destroying anything it protects here would
                // be a real still-in-use hazard. Leave everything alone; a hung device makes the leak moot.
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "wait geometry arena growth fence: vkWaitForFences timed out.");
            }

            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
        }

        if (arena.buffer) {
            // Every BLAS references the shared arena's device address. Growing either arena changes that
            // address, so lazily rebuild all mesh BLAS before the next ray-query frame.
            for (MeshResource &mesh_resource : meshes_) {
                if (mesh_resource.bottom_level_acceleration_structure) {
                    device->destroy_acceleration_structure(mesh_resource.bottom_level_acceleration_structure);
                    mesh_resource.bottom_level_acceleration_structure = {};
                }
            }
            device->destroy_buffer(arena.buffer);
        }
        arena.buffer = *new_buffer;
        arena.capacity_bytes = new_capacity;
        return {};
    }

    Core::RendererResult Renderer::try_upload_mesh(MeshResource &mesh) {
        ZoneScopedN("Renderer::try_upload_mesh");
        RHI::RhiDevice *device = rhi_device();
        if (!device) {
            return {};
        }
        vertex_arena_.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Storage |
                              RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst;
        index_arena_.usage = RHI::BufferUsage::Index | RHI::BufferUsage::Storage |
                             RHI::BufferUsage::TransferSrc | RHI::BufferUsage::TransferDst;
        if (device->is_enabled(RHI::Feature::AccelerationStructures)) {
            vertex_arena_.usage |= RHI::BufferUsage::AccelerationStructureInput;
            index_arena_.usage |= RHI::BufferUsage::AccelerationStructureInput;
        }

        // Alive meshes always retain their authoritative replay payload.
        // Alive meshes always retain their authoritative replay payload. Reaching this branch means
        // internal state was corrupted rather than a caller-selected residency policy being unavailable.
        if (mesh.vertices.empty()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot upload or restore an alive mesh with no CPU replay data.");
        }

        const u64 vertex_bytes = static_cast<u64>(mesh.vertices.size() * sizeof(GeometryVertex));
        if (vertex_arena_.used_bytes + vertex_bytes > vertex_arena_.capacity_bytes) {
            if (Core::RendererResult grown =
                    grow_geometry_arena(vertex_arena_, vertex_arena_.used_bytes + vertex_bytes, "renderer vertex arena");
                !grown.has_value()) {
                if (grown.error().code == Core::GraphicsBackendErrorCode::Unsupported) {
                    return {};
                }
                return grown;
            }
        }
        auto vertex_write = device->write_buffer(
            vertex_arena_.buffer,
            vertex_arena_.used_bytes,
            std::as_bytes(span<const GeometryVertex>{mesh.vertices.data(), mesh.vertices.size()}));
        if (!vertex_write) {
            if (vertex_write.error().code == RHI::RhiErrorCode::Unsupported) {
                return {};
            }
            return unexpected(graphics_error_from_rhi(vertex_write.error(), "upload vertex buffer"));
        }
        mesh.vertex_offset = static_cast<u32>(vertex_arena_.used_bytes / sizeof(GeometryVertex));
        vertex_arena_.used_bytes += vertex_bytes;

        if (!mesh.indices.empty()) {
            const u64 index_bytes = static_cast<u64>(mesh.indices.size() * sizeof(u32));
            if (index_arena_.used_bytes + index_bytes > index_arena_.capacity_bytes) {
                if (Core::RendererResult grown =
                        grow_geometry_arena(index_arena_, index_arena_.used_bytes + index_bytes, "renderer index arena");
                    !grown.has_value()) {
                    if (grown.error().code == Core::GraphicsBackendErrorCode::Unsupported) {
                        return {};
                    }
                    return grown;
                }
            }
            auto index_write = device->write_buffer(
                index_arena_.buffer,
                index_arena_.used_bytes,
                std::as_bytes(span<const u32>{mesh.indices.data(), mesh.indices.size()}));
            if (!index_write) {
                if (index_write.error().code == RHI::RhiErrorCode::Unsupported) {
                    return {};
                }
                return unexpected(graphics_error_from_rhi(index_write.error(), "upload index buffer"));
            }
            mesh.index_offset = static_cast<u32>(index_arena_.used_bytes / sizeof(u32));
            index_arena_.used_bytes += index_bytes;
        }

        glm::vec3 bounds_min{std::numeric_limits<f32>::max()};
        glm::vec3 bounds_max{std::numeric_limits<f32>::lowest()};
        for (const GeometryVertex &vertex : mesh.vertices) {
            bounds_min = glm::min(bounds_min, vertex.position);
            bounds_max = glm::max(bounds_max, vertex.position);
        }
        mesh.bounds_center = (bounds_min + bounds_max) * 0.5f;
        mesh.bounds_radius = glm::length(bounds_max - mesh.bounds_center);

        mesh.gpu_resident = true;
        return {};
    }

    Core::GraphicsBackendError Renderer::graphics_error_from_rhi(const RHI::RhiError &error,
                                                                 const char *operation) {
        ZoneScopedN("Renderer::graphics_error_from_rhi");
        Core::GraphicsBackendErrorCode code = Core::GraphicsBackendErrorCode::OperationFailed;
        switch (error.code) {
            case RHI::RhiErrorCode::Unsupported: code = Core::GraphicsBackendErrorCode::Unsupported; break;
            case RHI::RhiErrorCode::OutOfMemory: code = Core::GraphicsBackendErrorCode::OutOfMemory; break;
            case RHI::RhiErrorCode::DeviceLost: code = Core::GraphicsBackendErrorCode::DeviceLost; break;
            case RHI::RhiErrorCode::SurfaceLost: code = Core::GraphicsBackendErrorCode::SurfaceLost; break;
            case RHI::RhiErrorCode::FullScreenExclusiveLost: code = Core::GraphicsBackendErrorCode::FullScreenExclusiveLost; break;
            case RHI::RhiErrorCode::InvalidArgument:
            case RHI::RhiErrorCode::NotReady:
            case RHI::RhiErrorCode::OperationFailed:
                code = Core::GraphicsBackendErrorCode::OperationFailed;
                break;
        }

        return Core::GraphicsBackendError{
            .code = code,
            .message = string(operation) + " failed through RHI: " + error.message,
        };
    }

} // namespace SFT::Renderer
