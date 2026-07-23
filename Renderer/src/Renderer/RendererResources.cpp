#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
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

using std::span;
using std::string;
using std::unexpected;

namespace SFT::Renderer {

    Core::RendererExpected<MeshHandle> Renderer::create_mesh(span<const GeometryVertex> vertices,
                                                             span<const u32> indices,
                                                             const char *label) {
        if (vertices.empty()) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                        "Cannot create a mesh with no vertices."});
        }

        MeshResource mesh{};
        mesh.handle = MeshHandle{static_cast<u64>(meshes_.size() + 1)};
        mesh.label = label ? label : "";
        mesh.vertices.assign(vertices.begin(), vertices.end());
        mesh.indices.assign(indices.begin(), indices.end());
        mesh.alive = true;

        if (Core::RendererResult upload = try_upload_mesh(mesh); !upload.has_value()) {
            return unexpected(upload.error());
        }

        meshes_.push_back(std::move(mesh));
        return meshes_.back().handle;
    }

    Core::RendererExpected<MeshHandle> Renderer::upload(Mesh &mesh) {
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
        MeshResource *resource = mesh(handle);
        if (!resource) {
            return;
        }

        // The mesh's bytes live in the shared vertex/index arenas (try_upload_mesh), not a dedicated
        // buffer this resource owns, so there's nothing to individually free here — only whole-arena
        // teardown (destroy_all_resources) or a real free-list allocator (not implemented yet; nothing
        // else in the engine reclaims evicted GPU memory either — see asset-manager.md) could reclaim
        // this range. Evicting a mesh here just stops it from being drawn/replayed on future growth.
        resource->vertices.clear();
        resource->indices.clear();
        resource->vertex_offset = 0;
        resource->index_offset = 0;
        resource->gpu_resident = false;
        resource->alive = false;
    }

    MeshResource *Renderer::mesh(MeshHandle handle) noexcept {
        if (!handle || handle.value > meshes_.size()) {
            return nullptr;
        }
        MeshResource &resource = meshes_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    const MeshResource *Renderer::mesh(MeshHandle handle) const noexcept {
        if (!handle || handle.value > meshes_.size()) {
            return nullptr;
        }
        const MeshResource &resource = meshes_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    Core::RendererExpected<MaterialHandle> Renderer::create_material(const char *label) {
        MaterialResource material{};
        material.handle = MaterialHandle{static_cast<u64>(materials_.size() + 1)};
        material.label = label ? label : "";
        material.alive = true;
        materials_.push_back(std::move(material));
        return materials_.back().handle;
    }

    void Renderer::destroy_material(MaterialHandle handle) noexcept {
        MaterialResource *resource = material(handle);
        if (!resource) {
            return;
        }
        resource->label.clear();
        resource->alive = false;
    }

    MaterialResource *Renderer::material(MaterialHandle handle) noexcept {
        if (!handle || handle.value > materials_.size()) {
            return nullptr;
        }
        MaterialResource &resource = materials_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    const MaterialResource *Renderer::material(MaterialHandle handle) const noexcept {
        if (!handle || handle.value > materials_.size()) {
            return nullptr;
        }
        const MaterialResource &resource = materials_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    void Renderer::destroy_all_resources() noexcept {
        if (shader_hot_reload_poll_) {
            (void)shader_hot_reload_poll_->wait();
            shader_hot_reload_poll_.reset();
        }
        shader_watcher_.reset();

        frame_draws_.clear();
        destroy_scene_gpu_resources();

        for (MaterialInstanceResource &resource : material_instances_) {
            if (resource.alive) {
                destroy_material_instance_gpu(resource);
                resource = {};
            }
        }
        material_instances_.clear();
        for (MaterialTemplateResource &resource : material_templates_) {
            if (resource.alive) {
                destroy_material_template_gpu(resource);
                resource = {};
            }
        }
        material_templates_.clear();

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
        for (TextureResource &resource : textures_) {
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
            resource = {};
        }
        textures_.clear();

        // Reclaim every window's in-flight frame slots and destroy each slot's (reusable) fence, then the
        // window's presentation resources. Safe: teardown runs after the destructor's wait_idle(), so no
        // slot's GPU work is still pending.
        auto window_surfaces_guard = window_surfaces_.lock();
        for (auto &record : *window_surfaces_guard) {
            if (RHI::RhiDevice *device = rhi_device()) {
                for (FrameInFlight &slot : record->frames_in_flight) {
                    reclaim_frame_slot(slot, true);
                    destroy_text_frame_resources(*device, slot.text_overlay_resources);
                    destroy_frame_bloom_targets(slot);
                    destroy_frame_composite_target(slot);
                    destroy_frame_shadow_targets(slot);
                    destroy_frame_deferred_targets(slot);
                    if (slot.fence) {
                        device->destroy_fence(slot.fence);
                    }
                }
            }
            record->frames_in_flight.clear();
            destroy_rhi_presentation_resources(*record);
        }
        // Frame-slot bloom bind groups reference bloom_'s cached layout and sampler, so they must be
        // destroyed above before the shared pipeline resources are torn down.
        destroy_bloom_resources();
        destroy_bloom_composite_resources();
        destroy_shadow_lighting_resources();
        destroy_custom_post_process_resources();
    }

    Core::RendererResult Renderer::grow_geometry_arena(GeometryArena &arena, u64 required_bytes, const char *label) {
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

        // Replay every already-resident mesh's retained CPU-side data at its existing offset — arena
        // offsets are append-only/stable across growth, so nothing needs renumbering, just recopying
        // into the new, bigger buffer. Only runs on a (rare, load-time) growth event.
        const bool is_vertex_arena = &arena == &vertex_arena_;
        for (const MeshResource &existing : meshes_) {
            if (!existing.alive || !existing.gpu_resident) {
                continue;
            }
            if (is_vertex_arena) {
                if (existing.vertices.empty()) {
                    continue;
                }
                auto write = device->write_buffer(*new_buffer,
                    static_cast<u64>(existing.vertex_offset) * sizeof(GeometryVertex),
                    std::as_bytes(span<const GeometryVertex>{existing.vertices.data(), existing.vertices.size()}));
                if (!write) {
                    device->destroy_buffer(*new_buffer);
                    return unexpected(graphics_error_from_rhi(write.error(), "replay vertex arena growth"));
                }
            } else {
                if (existing.indices.empty()) {
                    continue;
                }
                auto write = device->write_buffer(*new_buffer,
                    static_cast<u64>(existing.index_offset) * sizeof(u32),
                    std::as_bytes(span<const u32>{existing.indices.data(), existing.indices.size()}));
                if (!write) {
                    device->destroy_buffer(*new_buffer);
                    return unexpected(graphics_error_from_rhi(write.error(), "replay index arena growth"));
                }
            }
        }

        if (arena.buffer) {
            device->destroy_buffer(arena.buffer);
        }
        arena.buffer = *new_buffer;
        arena.capacity_bytes = new_capacity;
        return {};
    }

    Core::RendererResult Renderer::try_upload_mesh(MeshResource &mesh) {
        RHI::RhiDevice *device = rhi_device();
        if (!device) {
            return {};
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
        Core::GraphicsBackendErrorCode code = Core::GraphicsBackendErrorCode::OperationFailed;
        switch (error.code) {
            case RHI::RhiErrorCode::Unsupported: code = Core::GraphicsBackendErrorCode::Unsupported; break;
            case RHI::RhiErrorCode::OutOfMemory: code = Core::GraphicsBackendErrorCode::OutOfMemory; break;
            case RHI::RhiErrorCode::DeviceLost: code = Core::GraphicsBackendErrorCode::DeviceLost; break;
            case RHI::RhiErrorCode::SurfaceLost: code = Core::GraphicsBackendErrorCode::SurfaceLost; break;
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
