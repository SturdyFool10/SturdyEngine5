#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <glm/common.hpp>
#include <glm/geometric.hpp>
#pragma endregion

#include <Renderer/LensTessellation.hpp>
#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;

namespace SFT::Renderer {

    /// Creates a mesh from the supplied parameters.
    ///
    /// @param vertices `vertices` value used by the operation.
    /// @param indices `indices` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
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
        // A destroyed mesh's slot is reused (with a new generation in the handle), so making and dropping meshes does not grow the table.
        const bool reuse_slot = !free_mesh_slots_.empty();
        const u32 slot = reuse_slot ? free_mesh_slots_.back() : static_cast<u32>(meshes_.size());
        mesh.generation = reuse_slot ? meshes_[slot].generation : 1;
        mesh.handle = MeshHandle{(static_cast<u64>(mesh.generation) << 32) | (static_cast<u64>(slot) + 1)};
        mesh.label = label ? label : "";
        mesh.vertices.assign(vertices.begin(), vertices.end());
        mesh.indices.assign(indices.begin(), indices.end());
        mesh.vertex_count = static_cast<u32>(vertices.size());
        mesh.index_count = static_cast<u32>(indices.size());
        mesh.alive = true;

        if (Core::RendererResult upload = try_upload_mesh(mesh); !upload.has_value()) {
            return unexpected(upload.error());
        }


        const MeshHandle created = mesh.handle;
        if (reuse_slot) {
            free_mesh_slots_.pop_back();
            meshes_[slot] = std::move(mesh);
        } else {
            meshes_.push_back(std::move(mesh));
        }
        return created;
    }

    /// Uploads the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param mesh `mesh` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
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

    /// Destroys the mesh identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_mesh(MeshHandle handle) noexcept {
        ZoneScopedN("Renderer::destroy_mesh");
        MeshResource *resource = mesh(handle);
        if (!resource) {
            return;
        }

        if (resource->bottom_level_acceleration_structure) {


            wait_idle();
            if (RHI::RhiDevice *device = rhi_device()) {
                device->destroy_acceleration_structure(resource->bottom_level_acceleration_structure);
            }
            resource->bottom_level_acceleration_structure = {};
        }


        {
            auto skinning = skinning_.lock();
            if (const auto it = skinning->skins.find(handle.value); it != skinning->skins.end()) {
                if (RHI::RhiDevice *device = rhi_device()) {
                    wait_idle();
                    if (it->second.bind_buffer) device->destroy_buffer(it->second.bind_buffer);
                    if (it->second.influence_buffer) device->destroy_buffer(it->second.influence_buffer);
                    if (it->second.morph_offset_buffer) device->destroy_buffer(it->second.morph_offset_buffer);
                    if (it->second.morph_entry_buffer) device->destroy_buffer(it->second.morph_entry_buffer);
                }
                skinning->skins.erase(it);
            }
        }

        retire_mesh_geometry(*resource);
        vector<GeometryVertex>{}.swap(resource->vertices);
        vector<u32>{}.swap(resource->indices);
        resource->vertex_offset = 0;
        resource->index_offset = 0;
        resource->vertex_count = 0;
        resource->index_count = 0;
        resource->has_lens_variant = false;
        resource->lens_index_count = 0;
        resource->gpu_resident = false;
        resource->alive = false;
        resource->lens_vertex_count = 0;
        // The slot is free for the next mesh; its generation moves on so the handle just destroyed never matches it.
        resource->generation = resource->generation == std::numeric_limits<u32>::max() ? 1 : resource->generation + 1;
        free_mesh_slots_.push_back(static_cast<u32>((handle.value & 0xFFFFFFFFull) - 1));
    }

    Core::RendererResult Renderer::update_mesh_vertices(MeshHandle handle, span<const GeometryVertex> vertices) {
        ZoneScopedN("Renderer::update_mesh_vertices");
        MeshResource *resource = mesh(handle);
        if (resource == nullptr || !resource->gpu_resident || vertices.size() != resource->vertices.size()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "update_mesh_vertices needs a resident mesh and an unchanged vertex count.");
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !vertex_arena_.buffer) {
            return {};
        }
        auto write = device->write_buffer(vertex_arena_.buffer,
                                          static_cast<u64>(resource->vertex_offset) * sizeof(GeometryVertex),
                                          std::as_bytes(vertices));
        if (!write) {
            return unexpected(graphics_error_from_rhi(write.error(), "update mesh vertices"));
        }
        std::copy(vertices.begin(), vertices.end(), resource->vertices.begin());
        drop_lens_variant(*resource);
        glm::vec3 bounds_min{std::numeric_limits<f32>::max()};
        glm::vec3 bounds_max{std::numeric_limits<f32>::lowest()};
        for (const GeometryVertex &vertex : vertices) {
            bounds_min = glm::min(bounds_min, vertex.position);
            bounds_max = glm::max(bounds_max, vertex.position);
        }
        resource->bounds_center = (bounds_min + bounds_max) * 0.5f;
        resource->bounds_radius = glm::length(bounds_max - resource->bounds_center);
        return {};
    }

    /// Performs the mesh operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    MeshResource *Renderer::mesh(MeshHandle handle) noexcept {
        ZoneScopedN("Renderer::mesh");
        const u64 slot = handle.value & 0xFFFFFFFFull;
        if (!handle || slot == 0 || slot > meshes_.size()) {
            return nullptr;
        }
        MeshResource &resource = meshes_[static_cast<usize>(slot - 1)];
        return resource.alive && resource.generation == static_cast<u32>(handle.value >> 32) ? &resource : nullptr;
    }

    /// Performs the mesh operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const MeshResource *Renderer::mesh(MeshHandle handle) const noexcept {
        ZoneScopedN("Renderer::mesh");
        const u64 slot = handle.value & 0xFFFFFFFFull;
        if (!handle || slot == 0 || slot > meshes_.size()) {
            return nullptr;
        }
        const MeshResource &resource = meshes_[static_cast<usize>(slot - 1)];
        return resource.alive && resource.generation == static_cast<u32>(handle.value >> 32) ? &resource : nullptr;
    }

    /// Creates a material from the supplied parameters.
    ///
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<MaterialHandle> Renderer::create_material(const char *label) {
        ZoneScopedN("Renderer::create_material");
        MaterialResource material{};
        material.handle = MaterialHandle{static_cast<u64>(materials_.size() + 1)};
        material.label = label ? label : "";
        material.alive = true;
        materials_.push_back(std::move(material));
        return materials_.back().handle;
    }

    /// Destroys the material identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_material(MaterialHandle handle) noexcept {
        ZoneScopedN("Renderer::destroy_material");
        MaterialResource *resource = material(handle);
        if (!resource) {
            return;
        }
        resource->label.clear();
        resource->alive = false;
    }

    /// Performs the material operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    MaterialResource *Renderer::material(MaterialHandle handle) noexcept {
        ZoneScopedN("Renderer::material");
        if (!handle || handle.value > materials_.size()) {
            return nullptr;
        }
        MaterialResource &resource = materials_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    /// Performs the material operation for `Renderer` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const MaterialResource *Renderer::material(MaterialHandle handle) const noexcept {
        ZoneScopedN("Renderer::material");
        if (!handle || handle.value > materials_.size()) {
            return nullptr;
        }
        const MaterialResource &resource = materials_[static_cast<usize>(handle.value - 1)];
        return resource.alive ? &resource : nullptr;
    }

    /// Destroys the all resources identified by the supplied parameters.
    ///
    /// @return Returns the current destroy all resources value.
    /// @note This function does not throw exceptions.
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

        destroy_object_history_resources();
        destroy_instance_cull_resources();
        destroy_displacement_mesh_resources();
        for (MaterialTemplateResource &resource : material_templates_) {
            if (resource.alive) {
                destroy_material_template_gpu(resource);
                resource = {};
            }
        }
        material_templates_.clear();

        destroy_deferred_msaa_resources();

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
        reset_geometry_allocator();
        for (MaterialResource &resource : materials_) {
            resource.alive = false;
            resource.label.clear();
        }


        destroy_all_offscreen_render_targets();
        destroy_retired_texture_gpu_now();
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


        auto window_surfaces_guard = window_surfaces_.lock();
        for (auto &record : *window_surfaces_guard) {
            if (RHI::RhiDevice *device = rhi_device()) {
                for (FrameInFlight &slot : record->frames_in_flight) {
                    reclaim_frame_slot(slot, true);
                    destroy_frame_shadow_targets(slot);
                    destroy_frame_atmosphere_targets(slot);
                    destroy_frame_deferred_targets(slot);
                    destroy_gtao_depth_pyramid(slot.gtao_depth_pyramid);
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


        destroy_shadow_lighting_resources();
        destroy_spectral_path_tracing_resources();
        destroy_custom_post_process_resources();
        destroy_custom_compute_effect_resources();
        destroy_skinning_gpu_resources();
        destroy_compute_kernels();
        destroy_atmosphere_lut_resources();
        destroy_hiz_build_resources();
        destroy_restir_gi_resources();
        auto_exposure_history_.release(rhi_device());
        history_textures_.release(rhi_device());
        destroy_svgf_resources();
        destroy_gtao_resources();
    }

    /// Grows geometry arena using the supplied arguments and current state.
    ///
    /// @param arena `arena` value used by the operation.
    /// @param required_bytes `required_bytes` value used by the operation.
    /// @param label `label` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::grow_geometry_arena(GeometryArena &arena, u64 required_bytes, const char *label) {
        ZoneScopedN("Renderer::grow_geometry_arena");
        RHI::RhiDevice *device = rhi_device();
        if (!device) {
            return {};
        }
        u64 new_capacity = std::max<u64>(required_bytes, std::max<u64>(arena.capacity_bytes * 2, 1u << 20));
        Foundation::log_info("Growing '{}': capacity {} -> {} bytes ({} in use, {} required).", label, arena.capacity_bytes,
                             new_capacity, arena.used_bytes, required_bytes);
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


                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "wait geometry arena growth fence: vkWaitForFences timed out.");
            }

            device->destroy_fence(*fence);
            device->destroy_command_buffer(*command_buffer);
        }

        if (arena.buffer) {


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

    /// Attempts to upload mesh without requiring normal failure to be exceptional.
    ///
    /// @param mesh `mesh` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`, `GraphicsBackendErrorCode::Unsupported`, `RhiErrorCode::Unsupported`.
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


        if (mesh.vertices.empty()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Cannot upload or restore an alive mesh with no CPU replay data.");
        }

        const u64 vertex_bytes = static_cast<u64>(mesh.vertices.size() * sizeof(GeometryVertex));
        const u64 index_bytes = static_cast<u64>(mesh.indices.size() * sizeof(u32));

        // Freed ranges are reused first. When the arena would have to grow although a quarter of it or more is free (in pieces too
        // small to use), the live meshes are packed together instead. Decided before this mesh takes any space: packing moves what
        // the arena holds.
        if (!compacting_geometry_arenas_ && (geometry_fragmented(false, vertex_bytes) || geometry_fragmented(true, index_bytes))) {
            compact_geometry_arenas();
        }

        const Core::RendererExpected<u64> vertex_at = allocate_geometry(false, vertex_bytes, "renderer vertex arena");
        if (!vertex_at.has_value()) {
            if (vertex_at.error().code == Core::GraphicsBackendErrorCode::Unsupported) {
                return {};
            }
            return unexpected(vertex_at.error());
        }
        auto vertex_write = device->write_buffer(
            vertex_arena_.buffer,
            *vertex_at,
            std::as_bytes(span<const GeometryVertex>{mesh.vertices.data(), mesh.vertices.size()}));
        if (!vertex_write) {
            retire_range_now(false, *vertex_at, vertex_bytes);
            if (vertex_write.error().code == RHI::RhiErrorCode::Unsupported) {
                return {};
            }
            return unexpected(graphics_error_from_rhi(vertex_write.error(), "upload vertex buffer"));
        }
        mesh.vertex_offset = static_cast<u32>(*vertex_at / sizeof(GeometryVertex));

        if (!mesh.indices.empty()) {
            const Core::RendererExpected<u64> index_at = allocate_geometry(true, index_bytes, "renderer index arena");
            if (!index_at.has_value()) {
                retire_range_now(false, *vertex_at, vertex_bytes);
                if (index_at.error().code == Core::GraphicsBackendErrorCode::Unsupported) {
                    return {};
                }
                return unexpected(index_at.error());
            }
            auto index_write = device->write_buffer(
                index_arena_.buffer,
                *index_at,
                std::as_bytes(span<const u32>{mesh.indices.data(), mesh.indices.size()}));
            if (!index_write) {
                retire_range_now(false, *vertex_at, vertex_bytes);
                retire_range_now(true, *index_at, index_bytes);
                if (index_write.error().code == RHI::RhiErrorCode::Unsupported) {
                    return {};
                }
                return unexpected(graphics_error_from_rhi(index_write.error(), "upload index buffer"));
            }
            mesh.index_offset = static_cast<u32>(*index_at / sizeof(u32));
        }

        glm::vec3 bounds_min{std::numeric_limits<f32>::max()};
        glm::vec3 bounds_max{std::numeric_limits<f32>::lowest()};
        for (const GeometryVertex &vertex : mesh.vertices) {
            bounds_min = glm::min(bounds_min, vertex.position);
            bounds_max = glm::max(bounds_max, vertex.position);
        }
        mesh.bounds_center = (bounds_min + bounds_max) * 0.5f;
        mesh.bounds_radius = glm::length(bounds_max - mesh.bounds_center);

        // The camera-lens variant is not made here: it is thousands of triangles for even a quad, and only a camera under the vertex-warp
        // lens draws it. `ensure_lens_variants` makes it for the meshes such a frame draws.
        mesh.has_lens_variant = false;
        mesh.lens_vertex_count = 0;
        mesh.lens_variant_tried = false;

        mesh.gpu_resident = true;
        return {};
    }

    void Renderer::ensure_lens_variants(span<const RenderItem> items) {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return;
        }
        for (const RenderItem &item : items) {
            MeshResource *mesh_resource = mesh(item.mesh);
            if (mesh_resource == nullptr || !mesh_resource->gpu_resident || mesh_resource->lens_variant_tried ||
                mesh_resource->skinned || mesh_resource->indices.empty()) {
                continue;
            }
            MeshResource &mesh = *mesh_resource;
            mesh.lens_variant_tried = true;
            // Edges are cut relative to the mesh's own size: local units say nothing about world scale (Sponza's
            // are not metres), and the error the split bounds is proportional to how much of the screen an edge spans.
            const f32 lens_edge = std::max(mesh.bounds_radius / kLensEdgesPerMeshRadius, 1.0e-4f);
            const usize lens_triangle_budget = std::max<usize>(mesh.indices.size() / 3 * 6, 50'000);
            const LensTessellatedMesh lens = lens_tessellate(mesh.vertices, mesh.indices, lens_edge, lens_triangle_budget);
            if (!lens.subdivided) {
                continue;
            }
            const u64 lens_vertex_bytes = static_cast<u64>(lens.vertices.size() * sizeof(GeometryVertex));
            const u64 lens_index_bytes = static_cast<u64>(lens.indices.size() * sizeof(u32));
            const Core::RendererExpected<u64> vertex_at = allocate_geometry(false, lens_vertex_bytes, "renderer vertex arena");
            if (!vertex_at.has_value()) {
                continue;
            }
            const Core::RendererExpected<u64> index_at = allocate_geometry(true, lens_index_bytes, "renderer index arena");
            if (!index_at.has_value()) {
                retire_range_now(false, *vertex_at, lens_vertex_bytes);
                continue;
            }
            const bool written =
                device->write_buffer(vertex_arena_.buffer, *vertex_at,
                                     std::as_bytes(span<const GeometryVertex>{lens.vertices.data(), lens.vertices.size()}))
                    .has_value() &&
                device->write_buffer(index_arena_.buffer, *index_at,
                                     std::as_bytes(span<const u32>{lens.indices.data(), lens.indices.size()}))
                    .has_value();
            if (!written) {
                retire_range_now(false, *vertex_at, lens_vertex_bytes);
                retire_range_now(true, *index_at, lens_index_bytes);
                continue;
            }
            mesh.lens_vertex_offset = static_cast<u32>(*vertex_at / sizeof(GeometryVertex));
            mesh.lens_vertex_count = static_cast<u32>(lens.vertices.size());
            mesh.lens_index_offset = static_cast<u32>(*index_at / sizeof(u32));
            mesh.lens_index_count = static_cast<u32>(lens.indices.size());
            mesh.has_lens_variant = true;
        }
    }

    Core::RendererExpected<u64> Renderer::allocate_geometry(bool index, u64 bytes, const char *label) {
        auto state = geometry_allocator_.lock();
        GeometryArena &arena = index ? index_arena_ : vertex_arena_;
        GeometryRangeSet &free_ranges = index ? state->index : state->vertex;
        if (const std::optional<u64> reused = free_ranges.allocate(bytes)) {
            return *reused;
        }
        if (arena.used_bytes + bytes > arena.capacity_bytes) {
            if (Core::RendererResult grown = grow_geometry_arena(arena, arena.used_bytes + bytes, label); !grown.has_value()) {
                return unexpected(grown.error());
            }
        }
        const u64 offset = arena.used_bytes;
        arena.used_bytes += bytes;
        return offset;
    }

    bool Renderer::geometry_fragmented(bool index, u64 bytes) {
        auto state = geometry_allocator_.lock();
        const GeometryArena &arena = index ? index_arena_ : vertex_arena_;
        const GeometryRangeSet &free_ranges = index ? state->index : state->vertex;
        if (bytes == 0 || free_ranges.fits(bytes) || arena.used_bytes + bytes <= arena.capacity_bytes) {
            return false;
        }
        return arena.capacity_bytes > 0 && free_ranges.free_bytes() * 4 >= arena.capacity_bytes;
    }

    void Renderer::retire_mesh_geometry(const MeshResource &mesh) {
        if (!mesh.gpu_resident) {
            return;
        }
        auto state = geometry_allocator_.lock();
        const u64 epoch = state->epoch;
        const auto retire = [&](bool index, u64 offset_units, u64 count, u64 unit) {
            if (count > 0) {
                state->retired.push_back(GeometryRange{.index = index, .offset = offset_units * unit, .bytes = count * unit, .epoch = epoch});
            }
        };
        retire(false, mesh.vertex_offset, mesh.vertex_count, sizeof(GeometryVertex));
        retire(true, mesh.index_offset, mesh.index_count, sizeof(u32));
        if (mesh.has_lens_variant) {
            retire(false, mesh.lens_vertex_offset, mesh.lens_vertex_count, sizeof(GeometryVertex));
            retire(true, mesh.lens_index_offset, mesh.lens_index_count, sizeof(u32));
        }
    }

    void Renderer::retire_range_now(bool index, u64 offset, u64 bytes) {
        auto state = geometry_allocator_.lock();
        GeometryArena &arena = index ? index_arena_ : vertex_arena_;
        (index ? state->index : state->vertex).release(offset, bytes, arena.used_bytes);
    }

    void Renderer::drop_lens_variant(MeshResource &mesh) {
        if (mesh.has_lens_variant) {
            auto state = geometry_allocator_.lock();
            const u64 epoch = state->epoch;
            if (mesh.lens_vertex_count > 0) {
                state->retired.push_back(GeometryRange{.index = false,
                                                       .offset = static_cast<u64>(mesh.lens_vertex_offset) * sizeof(GeometryVertex),
                                                       .bytes = static_cast<u64>(mesh.lens_vertex_count) * sizeof(GeometryVertex),
                                                       .epoch = epoch});
            }
            if (mesh.lens_index_count > 0) {
                state->retired.push_back(GeometryRange{.index = true,
                                                       .offset = static_cast<u64>(mesh.lens_index_offset) * sizeof(u32),
                                                       .bytes = static_cast<u64>(mesh.lens_index_count) * sizeof(u32),
                                                       .epoch = epoch});
            }
        }
        mesh.has_lens_variant = false;
        mesh.lens_vertex_count = 0;
        mesh.lens_index_count = 0;
        // Made again, from the new vertices, the next time a lens camera draws the mesh.
        mesh.lens_variant_tried = false;
    }

    void Renderer::move_retired_geometry_to_slot(FrameInFlight &slot) {
        auto state = geometry_allocator_.lock();
        slot.retired_geometry.insert(slot.retired_geometry.end(), state->retired.begin(), state->retired.end());
        state->retired.clear();
    }

    void Renderer::release_geometry_ranges(const vector<GeometryRange> &ranges) {
        if (ranges.empty()) {
            return;
        }
        auto state = geometry_allocator_.lock();
        for (const GeometryRange &range : ranges) {
            if (range.epoch != state->epoch) {
                continue;
            }
            GeometryArena &arena = range.index ? index_arena_ : vertex_arena_;
            (range.index ? state->index : state->vertex).release(range.offset, range.bytes, arena.used_bytes);
        }
    }

    void Renderer::release_retired_geometry_now() {
        vector<GeometryRange> ranges;
        {
            auto state = geometry_allocator_.lock();
            ranges.swap(state->retired);
        }
        release_geometry_ranges(ranges);
    }

    void Renderer::reset_geometry_allocator() noexcept {
        auto state = geometry_allocator_.lock();
        state->vertex.reset();
        state->index.reset();
        state->retired.clear();
        ++state->epoch;
    }

    void Renderer::compact_geometry_arenas() {
        ZoneScopedN("Renderer::compact_geometry_arenas");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || compacting_geometry_arenas_) {
            return;
        }
        // Frames in flight read the arenas at the old offsets; settle them before anything moves.
        wait_idle();
        compacting_geometry_arenas_ = true;
        for (MeshResource &mesh : meshes_) {
            if (mesh.bottom_level_acceleration_structure) {
                device->destroy_acceleration_structure(mesh.bottom_level_acceleration_structure);
                mesh.bottom_level_acceleration_structure = {};
            }
        }
        const u64 before = vertex_arena_.used_bytes + index_arena_.used_bytes;
        vertex_arena_.used_bytes = 0;
        index_arena_.used_bytes = 0;
        // Ranges retired earlier (some still held by frame slots) are of the layout being thrown away; the new epoch makes them void.
        reset_geometry_allocator();
        usize moved = 0;
        usize failed = 0;
        for (MeshResource &mesh : meshes_) {
            if (!mesh.alive || !mesh.gpu_resident) {
                continue;
            }
            mesh.gpu_resident = false;
            const Core::RendererResult uploaded = try_upload_mesh(mesh);
            if (uploaded.has_value() && mesh.gpu_resident) {
                ++moved;
            } else {
                ++failed;
                Foundation::log_error("Geometry arena compaction could not move mesh '{}': {}", mesh.label.c_str(),
                                      uploaded.has_value() ? std::string{"the upload did not complete"} : uploaded.error().message);
            }
        }
        compacting_geometry_arenas_ = false;
        Foundation::log_info("Compacted the geometry arenas: {} live mesh(es) moved{}, {} -> {} bytes in use.", moved,
                             failed > 0 ? std::format(" ({} failed)", failed) : std::string{}, before,
                             vertex_arena_.used_bytes + index_arena_.used_bytes);
    }

    /// Performs the graphics error from RHI operation for `Renderer` using the supplied arguments.
    ///
    /// @param error Error value describing the failure.
    /// @param operation `operation` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
