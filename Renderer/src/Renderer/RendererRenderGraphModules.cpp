#include <Foundation/Foundation.hpp>

#include <glm/trigonometric.hpp>

#include <Renderer/Renderer.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Renderer {

    namespace {

        /// Performs the missing module texture operation for `Renderer` using the supplied arguments.
        ///
        /// @param module `module` value used by the operation.
        /// @param semantic `semantic` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        [[nodiscard]] Core::RendererResult missing_module_texture(const ustr &module, const ustr &semantic) {
            UString message{module};
            message.append(" requires the render-graph semantic texture '"_ustr);
            message.append(semantic);
            message.append("'."_ustr);
            return Core::graphics_backend_error(Core::GraphicsBackendErrorCode::OperationFailed, message.cpp_string());
        }

    } // namespace

    /// Builds deferred MSAA module.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param submission `submission` value used by the operation.
    /// @param samples `samples` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::build_deferred_msaa_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        RHI::SampleCount samples) {
        ZoneScopedN("Renderer::build_deferred_msaa_module");
        using RenderGraphSemantics::RasterVisibilityDepth;
        using RenderGraphSemantics::ResolvedSceneDepth;
        using RenderGraphSemantics::ReusableSceneHdrScratch;
        using RenderGraphSemantics::SceneHdrColor;

        const RenderGraphTextureHandle scene_color = context.resources.texture<SceneHdrColor>();
        const RenderGraphTextureHandle resolved_depth = context.resources.texture<ResolvedSceneDepth>();
        const RenderGraphTextureHandle raster_depth = context.resources.texture<RasterVisibilityDepth>();
        if (!scene_color) {
            return missing_module_texture("deferred MSAA reconstruction"_ustr, "SceneHdrColor"_ustr);
        }
        if (!resolved_depth) {
            return missing_module_texture("deferred MSAA reconstruction"_ustr, "ResolvedSceneDepth"_ustr);
        }
        if (!raster_depth) {
            return missing_module_texture("deferred MSAA reconstruction"_ustr, "RasterVisibilityDepth"_ustr);
        }

        RenderGraphTextureHandle destination = context.resources.texture<ReusableSceneHdrScratch>();
        if (!destination) {
            destination = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                         RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc |
                         RHI::TextureUsage::TransferDst,
                .label = "deferred MSAA reconstruction target",
            });
        }

        context.graph.add_render_pass("deferred MSAA reconstruction"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = scene_color})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = resolved_depth})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = raster_depth})
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.render_extent.x,
                .height = context.render_extent.y,
            })
            .set_execute([this, &submission, extent = context.render_extent, samples,
                          scene_color, resolved_depth, raster_depth](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_deferred_msaa_reconstruction(
                    pass,
                    graph_context.texture(scene_color).default_view,
                    graph_context.texture(resolved_depth).default_view,
                    graph_context.texture(raster_depth).default_view,
                    submission.deferred_formats.scene_color,
                    extent,
                    samples,
                    submission.camera.near_plane,
                    submission.camera.far_plane,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(destination);
        return {};
    }

    /// Builds post process aa module.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param submission `submission` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::build_post_process_aa_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission) {
        ZoneScopedN("Renderer::build_post_process_aa_module");
        using RenderGraphSemantics::ReusableSceneHdrScratch;
        using RenderGraphSemantics::SceneHdrColor;

        if (submission.render_graph.post_process_aa == 0) {
            return {};
        }
        const RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        if (!source) {
            return missing_module_texture("scene-linear spatial anti-aliasing"_ustr, "SceneHdrColor"_ustr);
        }

        RenderGraphTextureHandle destination = context.resources.texture<ReusableSceneHdrScratch>();
        if (!destination || destination == source) {
            destination = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                         RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc |
                         RHI::TextureUsage::TransferDst,
                .label = "scene-linear spatial anti-aliasing target",
            });
        }

        context.graph.add_render_pass("scene-linear spatial anti-aliasing"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = source})
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.render_extent.x,
                .height = context.render_extent.y,
            })
            .set_execute([this, &submission, source,
                          extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_post_process_aa(
                    pass,
                    graph_context.texture(source).default_view,
                    submission.deferred_formats.scene_color,
                    submission.render_graph,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(destination);
        return {};
    }

    /// Builds motion blur module.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param submission `submission` value used by the operation.
    /// @param motion_texture Full-resolution per-pixel motion vector render-graph texture (deferred G-buffer's
    ///     SV_Target4 — see gbuffer_geometry.slang).
    /// @param depth_texture Full-resolution scene depth render-graph texture.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::build_motion_blur_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        RenderGraphTextureHandle motion_texture,
        RenderGraphTextureHandle depth_texture) {
        ZoneScopedN("Renderer::build_motion_blur_module");
        using RenderGraphSemantics::SceneHdrColor;

        if (!submission.render_graph.motion_blur.enabled) {
            return {};
        }
        const RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        if (!source) {
            return missing_module_texture("motion blur"_ustr, "SceneHdrColor"_ustr);
        }
        if (!motion_texture || !depth_texture) {
            return Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed,
                "Motion blur requires both a motion-vector and a depth render-graph texture.");
        }

        const glm::uvec2 render_extent{context.render_extent.x, context.render_extent.y};
        const u32 tile_size = std::max(submission.render_graph.motion_blur.tile_size_px, 1u);
        const glm::uvec2 tile_extent{
            (render_extent.x + tile_size - 1) / tile_size,
            (render_extent.y + tile_size - 1) / tile_size,
        };

        constexpr RHI::TextureUsage velocity_usage =
            RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage;
        const RenderGraphTextureHandle tile_max_velocity = context.graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RG16Float,
            .extent = RHI::Extent3D{.width = tile_extent.x, .height = tile_extent.y, .depth_or_layers = 1},
            .usage = velocity_usage,
            .label = "motion blur tile-max velocity",
        });
        const RenderGraphTextureHandle dilated_velocity = context.graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RG16Float,
            .extent = RHI::Extent3D{.width = tile_extent.x, .height = tile_extent.y, .depth_or_layers = 1},
            .usage = velocity_usage,
            .label = "motion blur dilated velocity",
        });
        const RenderGraphTextureHandle destination = context.graph.create_texture(RenderGraphTextureDesc{
            .format = submission.deferred_formats.scene_color,
            .extent = context.render_texture_extent(),
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                     RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst,
            .label = "motion blur gather target",
        });

        context.graph.add_compute_pass("motion blur tile max"_ustr)
            .add_sampled_texture(motion_texture)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = tile_max_velocity, .read = false, .write = true})
            .set_execute([this, &submission, motion_texture, tile_max_velocity, render_extent, tile_size](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_motion_blur_tile_max(
                    graph_context.compute_pass(),
                    graph_context.texture(motion_texture).default_view,
                    graph_context.texture(tile_max_velocity).default_view,
                    render_extent,
                    tile_size,
                    submission.transient_bind_groups);
            });

        context.graph.add_compute_pass("motion blur neighbor max"_ustr)
            .add_sampled_texture(tile_max_velocity)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = dilated_velocity, .read = false, .write = true})
            .set_execute([this, &submission, tile_max_velocity, dilated_velocity, tile_extent](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_motion_blur_neighbor_max(
                    graph_context.compute_pass(),
                    graph_context.texture(tile_max_velocity).default_view,
                    graph_context.texture(dilated_velocity).default_view,
                    tile_extent,
                    submission.transient_bind_groups);
            });

        context.graph.add_compute_pass("motion blur gather"_ustr)
            .add_sampled_texture(source)
            .add_sampled_texture(motion_texture)
            .add_sampled_texture(depth_texture)
            .add_sampled_texture(dilated_velocity)
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = destination, .read = false, .write = true})
            .set_execute([this, &submission, source, motion_texture, depth_texture, dilated_velocity, destination,
                          render_extent](RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_motion_blur_gather(
                    graph_context.compute_pass(),
                    graph_context.texture(source).default_view,
                    graph_context.texture(motion_texture).default_view,
                    graph_context.texture(depth_texture).default_view,
                    graph_context.texture(dilated_velocity).default_view,
                    graph_context.texture(destination).default_view,
                    submission.render_graph,
                    render_extent,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(destination);
        return {};
    }

    /// Builds the surfel-GI render-graph module: screen-space seed, hash-grid clear/rebuild, ray-trace/
    /// temporal-accumulation, and screen-space irradiance resolve. When disabled, imports a 1x1 white dummy
    /// texture instead so the deferred shadow lighting pass's binding layout stays fixed either way.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param submission `submission` value used by the operation.
    /// @param slot Frame-in-flight slot (supplies the shared scene TLAS built earlier this frame).
    /// @param gbuffer_normal Full-resolution encoded G-buffer normal render-graph texture.
    /// @param depth_texture Full-resolution scene depth render-graph texture.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RenderGraphTextureHandle> Renderer::build_surfel_gi_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        FrameInFlight &slot,
        RenderGraphTextureHandle gbuffer_normal,
        RenderGraphTextureHandle depth_texture) {
        ZoneScopedN("Renderer::build_surfel_gi_module");
        const SurfelGiSettings &settings = submission.render_graph.surfel_gi;

        if (!settings.enabled) {
            auto default_texture = ensure_default_white_texture();
            if (!default_texture) return unexpected(default_texture.error());
            const TextureResource *white = texture(*default_texture);
            if (white == nullptr || !white->texture || !white->view) {
                return unexpected(Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Surfel GI fallback dummy texture is missing its RHI resources."));
            }
            return context.graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = white->texture,
                .default_view = white->view,
                .format = RHI::Format::RGBA8Unorm,
                .extent = RHI::Extent3D{.width = white->width, .height = white->height, .depth_or_layers = 1},
                .usage = RHI::TextureUsage::Sampled,
                .label = "surfel GI disabled dummy irradiance",
            });
        }

        if (Core::RendererResult ready = ensure_surfel_gi_resources(settings.max_surfels, settings.hash_grid_bucket_count);
            !ready.has_value()) {
            return unexpected(ready.error());
        }
        RHI::BufferHandle surfel_buffer{};
        RHI::BufferHandle grid_buffer{};
        RHI::BufferHandle alloc_cursor_buffer{};
        u32 surfel_capacity = 0;
        u32 grid_bucket_capacity = 0;
        {
            auto guard = surfel_gi_.lock();
            surfel_buffer = guard->surfel_buffer;
            grid_buffer = guard->grid_buffer;
            alloc_cursor_buffer = guard->alloc_cursor_buffer;
            surfel_capacity = guard->surfel_capacity;
            grid_bucket_capacity = guard->grid_bucket_capacity;
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(Core::graphics_backend_error(
                Core::GraphicsBackendErrorCode::OperationFailed, "Cannot build surfel GI without an RHI device."));
        }

        const glm::mat4 view_projection = submission.camera.projection * submission.camera.view;
        const DirectionalLight &sun = submission.lighting.sun;
        const glm::vec3 sun_direction = glm::length(sun.direction) > 1.0e-5f
            ? glm::normalize(sun.direction) : glm::vec3{0.0f, -1.0f, 0.0f};
        const SurfelGiFrameConstants constants{
            .inverse_view_projection = glm::inverse(view_projection),
            .camera_position_max_surfels = glm::vec4{submission.camera.world_position, static_cast<f32>(surfel_capacity)},
            .cascade_distances = settings.cascade_distances,
            .grid_params = glm::vec4{
                static_cast<f32>(grid_bucket_capacity),
                std::max(settings.surfel_radius_near, 0.01f) * 2.0f,
                settings.surfel_radius_near,
                settings.surfel_radius_far,
            },
            .spawn_params = glm::vec4{
                static_cast<f32>(settings.max_surfels_spawned_per_frame),
                settings.screen_coverage_target,
                static_cast<f32>(settings.rays_per_surfel_per_frame),
                settings.max_ray_distance,
            },
            .temporal_intensity = glm::vec4{
                settings.temporal_accumulation_alpha,
                settings.intensity,
                settings.irradiance_sample_radius_multiplier,
                static_cast<f32>(settings.cascade_count),
            },
            .extent_frame_index = glm::vec4{
                static_cast<f32>(context.render_extent.x),
                static_cast<f32>(context.render_extent.y),
                static_cast<f32>(submission.frame_index),
                0.0f,
            },
            .sun_direction_angular_radius = glm::vec4{
                sun_direction, glm::radians(std::clamp(sun.angular_radius_degrees, 0.0f, 10.0f))},
            .sun_radiance = glm::vec4{glm::max(sun.radiance, glm::vec3{0.0f}), 0.0f},
            .sky_color_intensity = glm::vec4{
                glm::vec3{submission.render_graph.background_color}, submission.render_graph.background_intensity},
        };
        auto constant_buffer = device->create_buffer(RHI::BufferDesc{
            .size = sizeof(constants),
            .usage = RHI::BufferUsage::Uniform,
            .memory = RHI::MemoryLocation::HostUpload,
            .label = "surfel GI frame constants",
        });
        if (!constant_buffer) {
            return unexpected(graphics_error_from_rhi(constant_buffer.error(), "create surfel GI frame constants"));
        }
        if (auto written = device->write_buffer(*constant_buffer, 0, std::as_bytes(span{&constants, 1})); !written) {
            device->destroy_buffer(*constant_buffer);
            return unexpected(graphics_error_from_rhi(written.error(), "write surfel GI frame constants"));
        }
        slot.transient_buffers.push_back(*constant_buffer);
        const RHI::BufferHandle constants_buffer = *constant_buffer;

        const RenderGraphBufferHandle surfel_buffer_handle = context.graph.import_buffer(RenderGraphImportedBufferDesc{
            .buffer = surfel_buffer,
            .size = static_cast<u64>(surfel_capacity) * sizeof(SurfelGpuData),
            .label = "surfel GI surfel buffer",
        });
        const RenderGraphBufferHandle grid_buffer_handle = context.graph.import_buffer(RenderGraphImportedBufferDesc{
            .buffer = grid_buffer,
            .size = static_cast<u64>(grid_bucket_capacity) * sizeof(SurfelGridCellGpuData),
            .label = "surfel GI grid buffer",
        });
        const RenderGraphBufferHandle alloc_cursor_handle = context.graph.import_buffer(RenderGraphImportedBufferDesc{
            .buffer = alloc_cursor_buffer,
            .size = sizeof(u32),
            .label = "surfel GI alloc cursor buffer",
        });

        const glm::uvec2 render_extent{context.render_extent.x, context.render_extent.y};
        const RenderGraphTextureHandle irradiance = context.graph.create_texture(RenderGraphTextureDesc{
            .format = RHI::Format::RGBA16Float,
            .extent = context.render_texture_extent(),
            .usage = RHI::TextureUsage::Sampled | RHI::TextureUsage::Storage,
            .label = "surfel GI irradiance",
        });

        const auto rw_buffer = [](RenderGraphBufferHandle buffer) {
            return RenderGraphBufferAccessDesc{
                .buffer = buffer, .stages = RHI::PipelineStage::ComputeShader,
                .access = RHI::AccessFlags::ShaderRead | RHI::AccessFlags::ShaderWrite, .read = true, .write = true,
            };
        };
        const auto ro_buffer = [](RenderGraphBufferHandle buffer) {
            return RenderGraphBufferAccessDesc{
                .buffer = buffer, .stages = RHI::PipelineStage::ComputeShader,
                .access = RHI::AccessFlags::ShaderRead, .read = true, .write = false,
            };
        };

        context.graph.add_compute_pass("surfel screen seed"_ustr)
            .add_sampled_texture(gbuffer_normal)
            .add_sampled_texture(depth_texture)
            .add_buffer(rw_buffer(surfel_buffer_handle))
            .add_buffer(ro_buffer(grid_buffer_handle))
            .add_buffer(rw_buffer(alloc_cursor_handle))
            .set_side_effect(true)
            .set_execute([this, &submission, gbuffer_normal, depth_texture, constants_buffer, render_extent](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_surfel_screen_seed(
                    graph_context.compute_pass(),
                    graph_context.texture(gbuffer_normal).default_view,
                    graph_context.texture(depth_texture).default_view,
                    constants_buffer,
                    submission.render_graph,
                    render_extent,
                    submission.transient_bind_groups);
            });

        context.graph.add_compute_pass("surfel grid clear"_ustr)
            .add_buffer(rw_buffer(grid_buffer_handle))
            .set_side_effect(true)
            .set_execute([this, &submission, constants_buffer](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_surfel_grid_clear(
                    graph_context.compute_pass(), constants_buffer, submission.render_graph,
                    submission.transient_bind_groups);
            });

        context.graph.add_compute_pass("surfel hash grid build"_ustr)
            .add_buffer(ro_buffer(surfel_buffer_handle))
            .add_buffer(rw_buffer(grid_buffer_handle))
            .set_side_effect(true)
            .set_execute([this, &submission, constants_buffer](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_surfel_hash_grid_build(
                    graph_context.compute_pass(), constants_buffer, submission.render_graph,
                    submission.transient_bind_groups);
            });

        context.graph.add_compute_pass("surfel ray trace"_ustr)
            .add_buffer(rw_buffer(surfel_buffer_handle))
            .set_side_effect(true)
            .set_execute([this, &submission, &slot, constants_buffer](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_surfel_ray_trace(
                    graph_context.compute_pass(), slot, constants_buffer, submission.render_graph,
                    submission.transient_bind_groups);
            });

        context.graph.add_compute_pass("surfel irradiance resolve"_ustr)
            .add_sampled_texture(gbuffer_normal)
            .add_sampled_texture(depth_texture)
            .add_buffer(ro_buffer(surfel_buffer_handle))
            .add_buffer(ro_buffer(grid_buffer_handle))
            .add_storage_texture(RenderGraphStorageTextureAccessDesc{.texture = irradiance, .read = false, .write = true})
            .set_execute([this, &submission, gbuffer_normal, depth_texture, irradiance, constants_buffer, render_extent](
                             RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                return record_surfel_irradiance_resolve(
                    graph_context.compute_pass(),
                    graph_context.texture(gbuffer_normal).default_view,
                    graph_context.texture(depth_texture).default_view,
                    graph_context.texture(irradiance).default_view,
                    constants_buffer,
                    submission.render_graph,
                    render_extent,
                    submission.transient_bind_groups);
            });

        return irradiance;
    }

    /// Builds custom graph stage.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param submission `submission` value used by the operation.
    /// @param stage `stage` value used by the operation.
    /// @param logical_textures Texture used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    Core::RendererResult Renderer::build_custom_graph_stage(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        PostProcessStage stage,
        span<RenderGraphTextureHandle> logical_textures) {
        ZoneScopedN("Renderer::build_custom_graph_stage");
        using RenderGraphSemantics::ReusableSceneHdrScratch;
        using RenderGraphSemantics::SceneHdrColor;

        RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        if (!source) {
            return missing_module_texture("custom graph stage"_ustr, "SceneHdrColor"_ustr);
        }

        constexpr RHI::TextureUsage custom_hdr_usage =
            RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
            RHI::TextureUsage::Storage | RHI::TextureUsage::TransferSrc | RHI::TextureUsage::TransferDst;
        const CustomGraphProgram &program = submission.render_graph.custom_graph;
        const auto concrete_texture = [logical_textures](LogicalRenderGraphTexture logical) {
            return logical && logical.index < logical_textures.size()
                ? logical_textures[logical.index]
                : RenderGraphTextureHandle{};
        };

        for (usize pass_index = 0; pass_index < program.passes.size(); ++pass_index) {
            const CustomGraphPass &custom_pass = program.passes[pass_index];
            if (custom_pass.stage != stage) {
                continue;
            }
            if (!custom_pass.output || custom_pass.output.index >= logical_textures.size()) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Custom graph pass has an invalid logical output texture.");
            }
            const RenderGraphTextureHandle input = concrete_texture(custom_pass.input);
            if (!input) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Custom graph pass input was not produced before the pass was lowered.");
            }

            const RenderGraphTextureHandle output = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = custom_hdr_usage,
                .label = "custom graph HDR target",
            });
            switch (custom_pass.kind) {
                case CustomGraphPassKind::RasterEffect:
                    context.graph.add_render_pass("custom graph raster effect"_ustr)
                        .add_color_attachment(RenderGraphColorAttachmentDesc{
                            .texture = output,
                            .load_op = RHI::LoadOp::DontCare,
                            .store_op = RHI::StoreOp::Store,
                        })
                        .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = input})
                        .set_render_area(RHI::Rect2D{
                            .x = 0,
                            .y = 0,
                            .width = context.render_extent.x,
                            .height = context.render_extent.y,
                        })
                        .set_execute([this, &submission, input, pass_index,
                                      extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                            RHI::RenderPassEncoder &pass = graph_context.render_pass();
                            pass.set_viewport(RHI::Viewport{
                                .width = static_cast<f32>(extent.x),
                                .height = static_cast<f32>(extent.y),
                                .min_depth = 0.0f,
                                .max_depth = 1.0f,
                            });
                            pass.set_scissor(RHI::Rect2D{
                                .x = 0,
                                .y = 0,
                                .width = extent.x,
                                .height = extent.y,
                            });
                            return record_custom_post_process(
                                pass,
                                graph_context.texture(input).default_view,
                                submission.deferred_formats.scene_color,
                                submission.render_graph.custom_graph.passes[pass_index].raster,
                                submission.transient_bind_groups);
                        });
                    break;
                case CustomGraphPassKind::ComputeEffect:
                    if (submission.deferred_formats.scene_color != RHI::Format::RGBA16Float) {
                        return Core::graphics_backend_error(
                            Core::GraphicsBackendErrorCode::OperationFailed,
                            "Custom compute effects currently require the default RGBA16Float scene HDR format.");
                    }
                    context.graph.add_compute_pass("custom graph compute effect"_ustr)
                        .add_sampled_texture(input)
                        .add_storage_texture(RenderGraphStorageTextureAccessDesc{
                            .texture = output,
                            .read = false,
                            .write = true,
                        })
                        .set_execute([this, &submission, input, output, pass_index,
                                      extent = context.render_extent](RenderGraphComputeContext &graph_context) -> Core::RendererResult {
                            return record_custom_compute_effect(
                                graph_context.compute_pass(),
                                graph_context.texture(input).default_view,
                                graph_context.texture(output).default_view,
                                extent,
                                submission.render_graph.custom_graph.passes[pass_index].compute,
                                submission.transient_bind_groups);
                        });
                    break;
                case CustomGraphPassKind::Copy:
                    context.graph.add_copy_pass(RenderGraphCopyDesc{
                        .source = input,
                        .destination = output,
                        .label = UString{"custom graph exact HDR copy"_ustr},
                    });
                    break;
            }
            logical_textures[custom_pass.output.index] = output;
        }

        const LogicalRenderGraphTexture selected = stage == PostProcessStage::BeforeBloom
            ? program.before_bloom_presentation_output
            : program.after_bloom_presentation_output;
        if (selected) {
            source = concrete_texture(selected);
            if (!source) {
                return Core::graphics_backend_error(
                    Core::GraphicsBackendErrorCode::OperationFailed,
                    "Custom graph presentation output was not produced in its declared HDR stage.");
            }
        }


        for (usize effect_index = 0; effect_index < submission.render_graph.custom_post_processes.size(); ++effect_index) {
            if (submission.render_graph.custom_post_processes[effect_index].stage != stage) {
                continue;
            }
            const RenderGraphTextureHandle input = source;
            const RenderGraphTextureHandle output = context.graph.create_texture(RenderGraphTextureDesc{
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = custom_hdr_usage,
                .label = "legacy custom HDR post-process target",
            });
            context.graph.add_render_pass("legacy custom HDR post-process"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = output,
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = input})
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = context.render_extent.x,
                    .height = context.render_extent.y,
                })
                .set_execute([this, &submission, input, effect_index,
                              extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(extent.x),
                        .height = static_cast<f32>(extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = extent.x,
                        .height = extent.y,
                    });
                    return record_custom_post_process(
                        pass,
                        graph_context.texture(input).default_view,
                        submission.deferred_formats.scene_color,
                        submission.render_graph.custom_post_processes[effect_index],
                        submission.transient_bind_groups);
                });
            source = output;
        }

        for (LogicalRenderGraphTexture logical_output : program.outputs) {
            const RenderGraphTextureHandle output = concrete_texture(logical_output);
            if (output) {
                context.graph.mark_output(output);
            }
        }
        context.resources.publish_texture<SceneHdrColor>(source);
        return {};
    }

    /// Builds bloom module.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param submission `submission` value used by the operation.
    /// @param frame_slot Binding or storage slot addressed by the operation.
    /// @param enabled Whether the associated behavior is enabled.
    /// @param bloom_format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::build_bloom_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        FrameInFlight &frame_slot,
        bool enabled,
        RHI::Format bloom_format) {
        ZoneScopedN("Renderer::build_bloom_module");
        using RenderGraphSemantics::ReusableSceneHdrScratch;
        using RenderGraphSemantics::SceneHdrColor;

        if (!enabled) {
            return {};
        }
        const RenderGraphTextureHandle scene_source = context.resources.texture<SceneHdrColor>();
        if (!scene_source) {
            return missing_module_texture("bloom"_ustr, "SceneHdrColor"_ustr);
        }

        const vector<Core::Extent2D> &bloom_extents = frame_slot.bloom_targets.extents;
        vector<RenderGraphTextureHandle> bloom_levels;
        bloom_levels.reserve(bloom_extents.size());
        for (usize level = 0; level < bloom_extents.size(); ++level) {
            bloom_levels.push_back(context.graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = frame_slot.bloom_targets.textures[level],
                .default_view = frame_slot.bloom_targets.views[level],
                .format = bloom_format,
                .extent = RHI::Extent3D{
                    .width = bloom_extents[level].x,
                    .height = bloom_extents[level].y,
                    .depth_or_layers = 1,
                },
                .mip_levels = 1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = "persistent fractional bloom level",
            }));
        }

        for (usize level = 0; level < bloom_extents.size(); ++level) {
            const RenderGraphTextureHandle source = level == 0 ? scene_source : bloom_levels[level - 1];
            const RHI::TextureViewHandle level_source_view = level == 0
                ? RHI::TextureViewHandle{}
                : frame_slot.bloom_targets.views[level - 1];
            const Core::Extent2D source_extent = level == 0
                ? context.render_extent
                : bloom_extents[level - 1];
            const Core::Extent2D destination_extent = bloom_extents[level];
            const RHI::BindGroupHandle cached_bind_group =
                frame_slot.bloom_targets.downsample_bind_groups[level];

            context.graph.add_render_pass("bloom downsample"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = bloom_levels[level],
                    .view = frame_slot.bloom_targets.views[level],
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = source})
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = destination_extent.x,
                    .height = destination_extent.y,
                })
                .set_execute([this, &submission, scene_source, level_source_view, source_extent,
                              destination_extent, level, cached_bind_group](
                                 RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(destination_extent.x),
                        .height = static_cast<f32>(destination_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = destination_extent.x,
                        .height = destination_extent.y,
                    });
                    RHI::TextureViewHandle source_view = level_source_view;
                    RHI::BindGroupHandle bind_group = cached_bind_group;
                    if (level == 0) {
                        source_view = graph_context.texture(scene_source).default_view;
                        auto dynamic_bind_group = create_bloom_source_bind_group(source_view);
                        if (!dynamic_bind_group.has_value()) {
                            return unexpected(dynamic_bind_group.error());
                        }
                        {
                            auto bind_groups_guard = transient_bind_groups_lock_.lock();
                            submission.transient_bind_groups.push_back(*dynamic_bind_group);
                        }
                        bind_group = *dynamic_bind_group;
                    }
                    return record_bloom_downsample(
                        pass,
                        source_view,
                        glm::vec2{
                            1.0f / static_cast<f32>(source_extent.x),
                            1.0f / static_cast<f32>(source_extent.y),
                        },
                        submission.render_graph,
                        glm::vec2{
                            0.5f * static_cast<f32>(source_extent.x) /
                                static_cast<f32>(destination_extent.x),
                            0.5f * static_cast<f32>(source_extent.y) /
                                static_cast<f32>(destination_extent.y),
                        },
                        level == 0,
                        bind_group);
                });
        }

        for (usize level = bloom_extents.size(); level-- > 1;) {
            const Core::Extent2D source_extent = bloom_extents[level];
            const Core::Extent2D destination_extent = bloom_extents[level - 1];
            const RHI::TextureViewHandle source_view = frame_slot.bloom_targets.views[level];
            const RHI::BindGroupHandle bind_group = frame_slot.bloom_targets.upsample_bind_groups[level];

            context.graph.add_render_pass("bloom upsample"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = bloom_levels[level - 1],
                    .view = frame_slot.bloom_targets.views[level - 1],
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = bloom_levels[level]})
                .set_render_area(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = destination_extent.x,
                    .height = destination_extent.y,
                })
                .set_execute([this, &submission, source_view, source_extent,
                              destination_extent, bind_group](RenderGraphContext &graph_context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = graph_context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(destination_extent.x),
                        .height = static_cast<f32>(destination_extent.y),
                        .min_depth = 0.0f,
                        .max_depth = 1.0f,
                    });
                    pass.set_scissor(RHI::Rect2D{
                        .x = 0,
                        .y = 0,
                        .width = destination_extent.x,
                        .height = destination_extent.y,
                    });
                    return record_bloom_upsample(
                        pass,
                        source_view,
                        glm::vec2{
                            1.0f / static_cast<f32>(source_extent.x),
                            1.0f / static_cast<f32>(source_extent.y),
                        },
                        submission.render_graph,
                        bind_group);
                });
        }

        const RenderGraphTextureHandle composite_destination =
            context.graph.import_texture(RenderGraphImportedTextureDesc{
                .texture = frame_slot.composite_target.texture,
                .default_view = frame_slot.composite_target.view,
                .format = submission.deferred_formats.scene_color,
                .extent = context.render_texture_extent(),
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled |
                         RHI::TextureUsage::TransferSrc,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
                .label = "bloom composite target",
            });
        const RenderGraphTextureHandle reconstructed_bloom = bloom_levels.front();
        context.graph.add_render_pass("bloom composite"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = composite_destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = scene_source})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = reconstructed_bloom})
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.render_extent.x,
                .height = context.render_extent.y,
            })
            .set_execute([this, &submission, scene_source, reconstructed_bloom,
                          extent = context.render_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_bloom_composite(
                    pass,
                    graph_context.texture(scene_source).default_view,
                    graph_context.texture(reconstructed_bloom).default_view,
                    submission.deferred_formats.scene_color,
                    submission.render_graph.bloom_intensity,
                    submission.render_graph.bloom_threshold > 0.0f,
                    submission.transient_bind_groups);
            });

        context.resources.publish_texture<SceneHdrColor>(composite_destination);
        return {};
    }

    /// Builds tonemap module.
    ///
    /// @param context Context that supplies state required by the operation.
    /// @param submission `submission` value used by the operation.
    /// @param presentation_format Format used for the resource, render target, or conversion.
    /// @param hdr_output `hdr_output` value used by the operation.
    /// @param hdr_color_space `hdr_color_space` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::build_tonemap_module(
        RenderGraphModuleBuildContext &context,
        FrameSubmission &submission,
        RHI::Format presentation_format,
        bool hdr_output,
        Core::HdrColorSpaceMode hdr_color_space) {
        ZoneScopedN("Renderer::build_tonemap_module");
        using RenderGraphSemantics::PresentationTarget;
        using RenderGraphSemantics::SceneHdrColor;

        const RenderGraphTextureHandle source = context.resources.texture<SceneHdrColor>();
        const RenderGraphTextureHandle destination = context.resources.texture<PresentationTarget>();
        if (!source) {
            return missing_module_texture("tone mapping"_ustr, "SceneHdrColor"_ustr);
        }
        if (!destination) {
            return missing_module_texture("tone mapping"_ustr, "PresentationTarget"_ustr);
        }

        submission.render_graph.tone_mapping_hdr_output = hdr_output;
        submission.render_graph.tone_mapping_hdr_color_space = hdr_color_space;
        context.graph.add_render_pass(submission.render_graph.tone_mapping ? "tonemap"_ustr : "present scene color"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = destination,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                .texture = source,
                .stages = RHI::PipelineStage::FragmentShader,
                .access = RHI::AccessFlags::ShaderRead,
            })
            .set_render_area(RHI::Rect2D{
                .x = 0,
                .y = 0,
                .width = context.presentation_extent.x,
                .height = context.presentation_extent.y,
            })
            .set_execute([this, &submission, source, presentation_format,
                          extent = context.presentation_extent](RenderGraphContext &graph_context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = graph_context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = static_cast<f32>(extent.x),
                    .height = static_cast<f32>(extent.y),
                    .min_depth = 0.0f,
                    .max_depth = 1.0f,
                });
                pass.set_scissor(RHI::Rect2D{
                    .x = 0,
                    .y = 0,
                    .width = extent.x,
                    .height = extent.y,
                });
                return record_tonemap(
                    pass,
                    graph_context.texture(source).default_view,
                    presentation_format,
                    submission.render_graph,
                    submission.transient_bind_groups);
            });
        return {};
    }

} // namespace SFT::Renderer
