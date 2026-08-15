#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <string>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    Core::RendererResult Renderer::recover_from_device_loss() {
        ZoneScopedN("Renderer::recover_from_device_loss");
        if (recovering_from_device_loss_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::DeviceLost,
                                                "Renderer is already recovering from GPU device loss.");
        }
        return rebuild_backend_from_create_info(recovery_create_info_, "GPU device loss");
    }

    Core::RendererResult Renderer::reconfigure_backend(const Core::RendererCreateInfo &create_info) {
        ZoneScopedN("Renderer::reconfigure_backend");
        if (recovering_from_device_loss_) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                "Renderer is already rebuilding its graphics backend.");
        }
        return rebuild_backend_from_create_info(create_info, "runtime settings change");
    }

    Core::RendererResult Renderer::rebuild_backend_from_create_info(const Core::RendererCreateInfo &create_info,
                                                                    const char *reason) {
        ZoneScopedN("Renderer::rebuild_backend_from_create_info");
        auto backend_operation = backend_operation_mutex_.lock();
        if (!initialized_ || create_info.window == nullptr || window_surfaces_.lock()->empty()) {
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::DeviceLost,
                                                "Cannot rebuild graphics backend before renderer initialization state is available.");
        }

        recovering_from_device_loss_ = true;
        // Drain queued presentation before invalidating the device pointer it references. The backend
        // operation guard above also excludes every window's frame recording/submission for the complete
        // rebuild, so no new submit or present can race wait_idle() and old-device destruction below.
        {
            auto guard = window_surfaces_.lock();
            for (auto &record_ptr : *guard) {
                (void)drain_pending_present(*record_ptr, nullptr);
            }
        }
        wait_idle();
        // DXGI permits only one flip-model swapchain per HWND. The generic invalidation path below
        // intentionally clears old handles without destroying them (normally appropriate for a lost
        // device), but an intentional backend/API reconstruction still has a live old device and must
        // release its native swapchains before a D3D12 replacement attempts CreateSwapChainForHwnd.
        if (RHI::RhiDevice *old_device = rhi_device()) {
            auto guard = window_surfaces_.lock();
            for (const auto &record : *guard) {
                if (record->rhi_swapchain.is_valid()) {
                    old_device->destroy_swapchain(record->rhi_swapchain);
                    record->rhi_swapchain = {};
                }
            }
        }
        invalidate_gpu_resource_handles_no_destroy();
        graphics_backend_.reset();
        graphics_backend_ = Core::create_engine_backend(create_info.backend);
        if (!graphics_backend_) {
            recovering_from_device_loss_ = false;
            initialized_ = false;
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::InitializationFailed,
                                                string("Failed to create a replacement graphics backend for ") + reason + ".");
        }

        recovery_create_info_ = create_info;
        Core::RendererExpected<Core::RenderSurfaceHandle> primary = graphics_backend_->initialize(recovery_create_info_);
        if (!primary) {
            recovering_from_device_loss_ = false;
            initialized_ = false;
            return unexpected(Core::GraphicsBackendError{
                .code = primary.error().code,
                .message = string("Failed to reinitialize graphics backend for ") + reason + ": " + primary.error().message,
            });
        }

        {
            auto guard = window_surfaces_.lock();
            for (auto &record_ptr : *guard) {
                WindowSurfaceRecord &record = *record_ptr;
                record.rhi_surface = {};
                record.rhi_swapchain = {};
                record.depth_texture = {};
                record.depth_view = {};
                record.swapchain_extent = {};
                record.rhi_swapchain_dirty = true;
                if (record.primary) {
                    record.surface = *primary;
                    continue;
                }
                if (record.window == nullptr) {
                    continue;
                }

                Core::RendererExpected<Core::RenderSurfaceHandle> recreated =
                    graphics_backend_->create_window_surface(*record.window, record.desired_frames_in_flight);
                if (!recreated) {
                    recovering_from_device_loss_ = false;
                    initialized_ = false;
                    return unexpected(Core::GraphicsBackendError{
                        .code = recreated.error().code,
                        .message = string("Failed to recreate renderer window surface for ") + reason + ": " + recreated.error().message,
                    });
                }
                record.surface = *recreated;
            }
        }

        capabilities_ = graphics_backend_->capabilities();
        Core::RendererResult restored = restore_gpu_resources_after_recovery();
        recovering_from_device_loss_ = false;
        if (!restored.has_value()) {
            initialized_ = false;
            return restored;
        }

        initialized_ = true;
        return {};
    }

    void Renderer::invalidate_gpu_resource_handles_no_destroy() noexcept {
        ZoneScopedN("Renderer::invalidate_gpu_resource_handles_no_destroy");
        // The old device (and every buffer it owned, including the shared vertex/index arenas) is
        // gone — reset the arenas to empty so restore_gpu_resources_after_recovery()'s per-mesh
        // try_upload_mesh() replay rebuilds them from scratch via the ordinary growth path.
        vertex_arena_ = GeometryArena{.usage = vertex_arena_.usage};
        index_arena_ = GeometryArena{.usage = index_arena_.usage};
        for (MeshResource &mesh : meshes_) {
            mesh.vertex_offset = 0;
            mesh.index_offset = 0;
            mesh.bottom_level_acceleration_structure = {};
            mesh.gpu_resident = false;
        }

        for (TextureResource &texture : textures_) {
            texture.texture = {};
            texture.view = {};
            texture.sampler = {};
        }
        // Preserve public target identities/descriptions and their borrowed TextureHandle slots, but
        // never let old-device RHI handles survive into the replacement backend.
        invalidate_offscreen_render_targets_after_device_loss();

        for (MaterialTemplateResource &material_template : material_templates_) {
            material_template.vertex_module = {};
            material_template.fragment_module = {};
            material_template.depth_only_fragment_module = {};
            material_template.depth_only_fragment_entry_point.clear();
            material_template.has_depth_only_fragment = false;
            material_template.bind_group_layouts.clear();
            material_template.bind_group_layout_sets.clear();
            material_template.pipeline_layout = {};
        }
        // Every template's cached pipeline handles above are invalid now the device is gone — see
        // material_pipeline_variants_'s doc comment (RendererModule.hpp) for why this lives as one
        // shared map instead of per-template storage; clearing it wholesale here is the equivalent of
        // the per-template pipeline_variants.clear() this loop used to do.
        material_pipeline_variants_.lock()->clear();
        depth_only_pipeline_variants_.lock()->clear();
        for (MaterialInstanceResource &material_instance : material_instances_) {
            for (MaterialInstanceFrame &frame : material_instance.frames) {
                frame.uniform_buffer = {};
                frame.bind_groups.clear();
                frame.uniform_dirty = true;
                frame.bind_groups_dirty = true;
            }
        }

        // The old device is gone — drop every in-flight frame slot's handles (command buffers, fences,
        // deferred transients) and scene GPU buffers without destroying them; the fresh device starts over.
        {
            auto guard = window_surfaces_.lock();
            for (auto &record : *guard) {
                record->frames_in_flight.clear();
                record->spectral_accumulation = {};
                record->scene_frame_resources.clear();
                record->hiz_pyramid = {};
            }
        }

        // Every handle cached below belonged to the destroyed device. Reset without calling ordinary
        // destroy functions: replacement-device pools restart their numeric IDs, so destroying an old
        // handle against the new device could accidentally destroy an unrelated colliding resource.
        *bloom_.lock() = {};
        *bloom_composite_.lock() = {};
        *shadow_lighting_.lock() = {};
        *deferred_msaa_.lock() = {};
        *tonemap_.lock() = {};
        *text_overlay_.lock() = {};
        custom_post_process_resources_.lock()->clear();
        custom_compute_effect_resources_.lock()->clear();
        *spectral_path_tracing_.lock() = {};
        *instance_cull_.lock() = {};
        instanced_pipeline_variants_.lock()->clear();
        *object_history_.lock() = {};
        object_history_pipeline_variants_.lock()->clear();
        *hiz_build_.lock() = {};
        *atmosphere_lut_.lock() = {};

        frame_draws_.clear();
    }

    Core::RendererResult Renderer::restore_gpu_resources_after_recovery() {
        ZoneScopedN("Renderer::restore_gpu_resources_after_recovery");
        for (MeshResource &mesh : meshes_) {
            if (!mesh.alive) {
                continue;
            }
            Core::RendererResult uploaded = try_upload_mesh(mesh);
            if (!uploaded.has_value()) {
                return uploaded;
            }
        }

        // Recreate ordinary renderer-owned textures under their stable public handles before any
        // material bind groups are rebuilt. Adopted/borrowed wrappers are restored by their owner (the
        // offscreen-target path immediately below) or by the external caller that owns the raw RHI state.
        for (TextureResource &texture : textures_) {
            if (!texture.alive || !texture.owns_gpu_resources) {
                continue;
            }
            if (Core::RendererResult restored = create_owned_texture_gpu(texture); !restored.has_value()) {
                return restored;
            }
        }

        // Recreate target backing in place before material bind groups are rebuilt below. Both the
        // OffscreenRenderTargetHandle and its borrowed TextureHandle remain stable across recovery.
        if (Core::RendererResult targets = restore_offscreen_render_targets_after_recovery();
            !targets.has_value()) {
            return targets;
        }

        // Already-cleared in invalidate_gpu_resource_handles_no_destroy() (see its doc comment), but
        // cheap and correct to ensure again here in case this is ever called without that first.
        material_pipeline_variants_.lock()->clear();
        for (MaterialTemplateResource &material_template : material_templates_) {
            if (!material_template.alive) {
                continue;
            }
            material_template.vertex_entry_point.clear();
            material_template.fragment_entry_point.clear();
            material_template.has_fragment = false;
            material_template.bind_group_layouts.clear();
            material_template.bind_group_layout_sets.clear();
            material_template.uniform_block_size = 0;
            material_template.uniform_set = 0;
            material_template.uniform_binding = 0;
            material_template.has_uniform_block = false;
            material_template.parameters.clear();
            material_template.texture_slots.clear();
            Core::RendererResult built = build_material_template_gpu(material_template, material_template.shader);
            if (!built.has_value()) {
                return built;
            }
        }

        for (MaterialInstanceResource &instance : material_instances_) {
            if (!instance.alive) {
                continue;
            }
            MaterialTemplateResource *tmpl = material_template(instance.material_template);
            if (tmpl == nullptr) {
                return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed,
                                                    "Cannot restore material instance after backend rebuild: template is missing.");
            }
            const vector<byte> previous_uniforms = instance.uniform_values;
            const vector<MaterialTextureBinding> previous_textures = instance.textures;
            instance.frames.clear();
            Core::RendererResult initialized = initialize_material_instance_state(instance, *tmpl);
            if (!initialized.has_value()) {
                return initialized;
            }
            if (previous_uniforms.size() == instance.uniform_values.size()) {
                instance.uniform_values = previous_uniforms;
            }
            if (previous_textures.size() == instance.textures.size()) {
                instance.textures = previous_textures;
            }
            for (MaterialInstanceFrame &frame : instance.frames) {
                frame.uniform_dirty = true;
                frame.bind_groups_dirty = true;
            }
        }
        return {};
    }

} // namespace SFT::Renderer
