module;

#pragma region Imports
#include <expected>
#include <span>
#include <string>
#include <utility>
#pragma endregion

module Sturdy.Renderer;

import :Renderer;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;

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

        if (RHI::RhiDevice *device = rhi_device()) {
            if (resource->vertex_buffer) {
                device->destroy_buffer(resource->vertex_buffer);
            }
            if (resource->index_buffer) {
                device->destroy_buffer(resource->index_buffer);
            }
        }

        resource->vertices.clear();
        resource->indices.clear();
        resource->vertex_buffer = {};
        resource->index_buffer = {};
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
        destroy_debug_scene_resources();
        frame_draws_.clear();

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

        if (RHI::RhiDevice *device = rhi_device()) {
            for (RHI::BindGroupHandle group : frame_transient_bind_groups_) {
                if (group) {
                    device->destroy_bind_group(group);
                }
            }
        }
        frame_transient_bind_groups_.clear();

        // Reclaim every in-flight frame slot and destroy its (reusable) fence. Safe: teardown runs after
        // the destructor's wait_idle(), so no slot's GPU work is still pending.
        if (RHI::RhiDevice *device = rhi_device()) {
            for (FrameInFlight &slot : frames_in_flight_) {
                reclaim_frame_slot(slot);
                if (slot.fence) {
                    device->destroy_fence(slot.fence);
                }
            }
        }
        frames_in_flight_.clear();
        destroy_tonemap_resources();

        for (MeshResource &resource : meshes_) {
            if (resource.alive) {
                destroy_mesh(resource.handle);
            }
        }
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
        for (WindowSurfaceRecord &record : window_surfaces_) {
            destroy_rhi_presentation_resources(record);
        }
    }

    Core::RendererResult Renderer::try_upload_mesh(MeshResource &mesh) {
        RHI::RhiDevice *device = rhi_device();
        if (!device) {
            return {};
        }

        const u64 vertex_bytes = static_cast<u64>(mesh.vertices.size() * sizeof(GeometryVertex));
        RHI::BufferDesc vertex_desc{
            .size = vertex_bytes,
            .usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::TransferDst,
            .memory = RHI::MemoryLocation::DeviceLocal,
            .label = mesh.label.empty() ? "renderer mesh vertex buffer" : mesh.label.c_str(),
        };
        auto vertex_buffer = device->create_buffer(vertex_desc);
        if (!vertex_buffer) {
            if (vertex_buffer.error().code == RHI::RhiErrorCode::Unsupported) {
                return {};
            }
            return unexpected(graphics_error_from_rhi(vertex_buffer.error(), "create vertex buffer"));
        }

        auto vertex_write = device->write_buffer(
            *vertex_buffer,
            0,
            std::as_bytes(span<const GeometryVertex>{mesh.vertices.data(), mesh.vertices.size()}));
        if (!vertex_write) {
            device->destroy_buffer(*vertex_buffer);
            if (vertex_write.error().code == RHI::RhiErrorCode::Unsupported) {
                return {};
            }
            return unexpected(graphics_error_from_rhi(vertex_write.error(), "upload vertex buffer"));
        }
        mesh.vertex_buffer = *vertex_buffer;

        if (!mesh.indices.empty()) {
            const u64 index_bytes = static_cast<u64>(mesh.indices.size() * sizeof(u32));
            RHI::BufferDesc index_desc{
                .size = index_bytes,
                .usage = RHI::BufferUsage::Index | RHI::BufferUsage::TransferDst,
                .memory = RHI::MemoryLocation::DeviceLocal,
                .label = mesh.label.empty() ? "renderer mesh index buffer" : mesh.label.c_str(),
            };
            auto index_buffer = device->create_buffer(index_desc);
            if (!index_buffer) {
                device->destroy_buffer(mesh.vertex_buffer);
                mesh.vertex_buffer = {};
                if (index_buffer.error().code == RHI::RhiErrorCode::Unsupported) {
                    return {};
                }
                return unexpected(graphics_error_from_rhi(index_buffer.error(), "create index buffer"));
            }

            auto index_write = device->write_buffer(
                *index_buffer,
                0,
                std::as_bytes(span<const u32>{mesh.indices.data(), mesh.indices.size()}));
            if (!index_write) {
                device->destroy_buffer(mesh.vertex_buffer);
                device->destroy_buffer(*index_buffer);
                mesh.vertex_buffer = {};
                if (index_write.error().code == RHI::RhiErrorCode::Unsupported) {
                    return {};
                }
                return unexpected(graphics_error_from_rhi(index_write.error(), "upload index buffer"));
            }
            mesh.index_buffer = *index_buffer;
        }

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
