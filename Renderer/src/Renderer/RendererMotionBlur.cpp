#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#pragma region Imports
#include <array>
#include <cstddef>
#include <span>
#include <vector>
#include <glm/vec2.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        /// Creates an error result describing the supplied motion-blur failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError motion_blur_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        struct TileMaxConstants {
            glm::uvec2 render_extent;
            u32 tile_size;
        };
        static_assert(sizeof(TileMaxConstants) == 12);

        struct NeighborMaxConstants {
            glm::uvec2 tile_extent;
        };
        static_assert(sizeof(NeighborMaxConstants) == 8);

        struct GatherConstants {
            glm::vec2 render_extent;
            f32 intensity;
            f32 shutter_fraction;
            f32 max_blur_radius_px;
            f32 fg_bg_weight_bias;
            u32 sample_count;
            u32 tile_size;
            u32 camera_motion_only;
        };
        static_assert(sizeof(GatherConstants) == 36);

        /// Finds the reflected binding with the supplied name, or reports a motion-blur error.
        ///
        /// @param resources Reflected resource bindings to search.
        /// @param name Name of the shader resource being resolved.
        /// @param shader_label Label used in the error message when the resource is missing.
        ///
        /// @return Returns the binding index on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] Core::RendererExpected<u32> find_binding(
            const vector<ReflectedResource> &resources, const char *name, const char *shader_label) {
            for (const ReflectedResource &resource : resources) {
                if (resource.name == name) {
                    return resource.binding;
                }
            }
            return unexpected(motion_blur_error(
                string(shader_label) + " shader reflection is missing the '" + name + "' resource."));
        }

    } // namespace

    /// Finds or creates the motion blur resources required by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_motion_blur_resources() {
        ZoneScopedN("Renderer::ensure_motion_blur_resources");
        auto guard = motion_blur_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(motion_blur_error("Cannot build motion-blur resources without an RHI device."));
        }

        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) return unexpected(shader_target.error());

        auto cleanup = [&]() noexcept {
            if (guard->gather_sampler) device->destroy_sampler(guard->gather_sampler);
            if (guard->gather_pipeline) device->destroy_compute_pipeline(guard->gather_pipeline);
            if (guard->gather_pipeline_layout) device->destroy_pipeline_layout(guard->gather_pipeline_layout);
            if (guard->gather_bind_group_layout) device->destroy_bind_group_layout(guard->gather_bind_group_layout);
            if (guard->gather_module) device->destroy_shader_module(guard->gather_module);
            if (guard->neighbor_max_pipeline) device->destroy_compute_pipeline(guard->neighbor_max_pipeline);
            if (guard->neighbor_max_pipeline_layout) device->destroy_pipeline_layout(guard->neighbor_max_pipeline_layout);
            if (guard->neighbor_max_bind_group_layout) device->destroy_bind_group_layout(guard->neighbor_max_bind_group_layout);
            if (guard->neighbor_max_module) device->destroy_shader_module(guard->neighbor_max_module);
            if (guard->tile_max_pipeline) device->destroy_compute_pipeline(guard->tile_max_pipeline);
            if (guard->tile_max_pipeline_layout) device->destroy_pipeline_layout(guard->tile_max_pipeline_layout);
            if (guard->tile_max_bind_group_layout) device->destroy_bind_group_layout(guard->tile_max_bind_group_layout);
            if (guard->tile_max_module) device->destroy_shader_module(guard->tile_max_module);
            *guard = {};
        };

        {
            const slang::ShaderCompileOptions options{
                .targets = shader_compile_targets_for_device(device),
                .entry_points = {slang::ShaderEntryPointRequest{.name = "tileMaxMain", .stage = slang::ShaderStage::Compute}},
            };
            slang::ShaderVariantCache shader_cache{
                slang::ShaderSource::from_file("Shaders/motion_blur_tile_max.slang", "motion_blur_tile_max"),
                options,
                slang::ShaderCompiler{},
                recovery_create_info_.enable_shader_disk_cache};
            auto shader = shader_cache.get_or_compile_base();
            if (!shader) {
                return unexpected(motion_blur_error("compile motion-blur tile-max shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
            }
            guard->tile_max_shader = *shader;

            auto code = guard->tile_max_shader.entry_point_code("tileMaxMain", shader_target->slang_target.format);
            if (!code) {
                return unexpected(motion_blur_error("generate motion-blur tile-max bytecode failed: " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = "motion blur tile-max compute module",
            });
            if (!module) {
                return unexpected(graphics_error_from_rhi(module.error(), "create motion-blur tile-max compute module"));
            }
            guard->tile_max_module = *module;

            const slang::ShaderReflection &reflection = guard->tile_max_shader.reflection();
            const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
            if (generated.empty()) {
                cleanup();
                return unexpected(motion_blur_error("motion-blur tile-max shader reflection produced no bind-group layout."));
            }
            auto layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
                .label = "motion blur tile-max bind group layout",
            });
            if (!layout) {
                cleanup();
                return unexpected(graphics_error_from_rhi(layout.error(), "create motion-blur tile-max bind group layout"));
            }
            guard->tile_max_bind_group_layout = *layout;

            const vector<ReflectedResource> resources = collect_resource_bindings(reflection);
            auto motion_binding = find_binding(resources, "gbufferMotion", "motion-blur tile-max");
            if (!motion_binding) { cleanup(); return unexpected(motion_binding.error()); }
            guard->tile_max_motion_binding = *motion_binding;
            auto output_binding = find_binding(resources, "tileMaxVelocityOut", "motion-blur tile-max");
            if (!output_binding) { cleanup(); return unexpected(output_binding.error()); }
            guard->tile_max_output_binding = *output_binding;

            const vector<RHI::PushConstantRange> push_constant_ranges =
                generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
            if (push_constant_ranges.empty()) {
                cleanup();
                return unexpected(motion_blur_error("motion-blur tile-max shader produced no push-constant range."));
            }
            const array<RHI::BindGroupLayoutHandle, 1> layouts{guard->tile_max_bind_group_layout};
            auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
                .label = "motion blur tile-max pipeline layout",
            });
            if (!pipeline_layout) {
                cleanup();
                return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create motion-blur tile-max pipeline layout"));
            }
            guard->tile_max_pipeline_layout = *pipeline_layout;

            auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                .layout = guard->tile_max_pipeline_layout,
                .compute = RHI::ShaderEntry{.module = guard->tile_max_module, .entry_point = "tileMaxMain", .stage = RHI::ShaderStage::Compute},
                .label = "motion blur tile-max pipeline",
            });
            if (!pipeline) {
                cleanup();
                return unexpected(graphics_error_from_rhi(pipeline.error(), "create motion-blur tile-max pipeline"));
            }
            guard->tile_max_pipeline = *pipeline;
        }

        {
            const slang::ShaderCompileOptions options{
                .targets = shader_compile_targets_for_device(device),
                .entry_points = {slang::ShaderEntryPointRequest{.name = "neighborMaxMain", .stage = slang::ShaderStage::Compute}},
            };
            slang::ShaderVariantCache shader_cache{
                slang::ShaderSource::from_file("Shaders/motion_blur_neighbor_max.slang", "motion_blur_neighbor_max"),
                options,
                slang::ShaderCompiler{},
                recovery_create_info_.enable_shader_disk_cache};
            auto shader = shader_cache.get_or_compile_base();
            if (!shader) {
                cleanup();
                return unexpected(motion_blur_error("compile motion-blur neighbor-max shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
            }
            guard->neighbor_max_shader = *shader;

            auto code = guard->neighbor_max_shader.entry_point_code("neighborMaxMain", shader_target->slang_target.format);
            if (!code) {
                cleanup();
                return unexpected(motion_blur_error("generate motion-blur neighbor-max bytecode failed: " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = "motion blur neighbor-max compute module",
            });
            if (!module) {
                cleanup();
                return unexpected(graphics_error_from_rhi(module.error(), "create motion-blur neighbor-max compute module"));
            }
            guard->neighbor_max_module = *module;

            const slang::ShaderReflection &reflection = guard->neighbor_max_shader.reflection();
            const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
            if (generated.empty()) {
                cleanup();
                return unexpected(motion_blur_error("motion-blur neighbor-max shader reflection produced no bind-group layout."));
            }
            auto layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
                .label = "motion blur neighbor-max bind group layout",
            });
            if (!layout) {
                cleanup();
                return unexpected(graphics_error_from_rhi(layout.error(), "create motion-blur neighbor-max bind group layout"));
            }
            guard->neighbor_max_bind_group_layout = *layout;

            const vector<ReflectedResource> resources = collect_resource_bindings(reflection);
            auto input_binding = find_binding(resources, "tileMaxVelocity", "motion-blur neighbor-max");
            if (!input_binding) { cleanup(); return unexpected(input_binding.error()); }
            guard->neighbor_max_input_binding = *input_binding;
            auto output_binding = find_binding(resources, "dilatedVelocityOut", "motion-blur neighbor-max");
            if (!output_binding) { cleanup(); return unexpected(output_binding.error()); }
            guard->neighbor_max_output_binding = *output_binding;

            const vector<RHI::PushConstantRange> push_constant_ranges =
                generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
            if (push_constant_ranges.empty()) {
                cleanup();
                return unexpected(motion_blur_error("motion-blur neighbor-max shader produced no push-constant range."));
            }
            const array<RHI::BindGroupLayoutHandle, 1> layouts{guard->neighbor_max_bind_group_layout};
            auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
                .label = "motion blur neighbor-max pipeline layout",
            });
            if (!pipeline_layout) {
                cleanup();
                return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create motion-blur neighbor-max pipeline layout"));
            }
            guard->neighbor_max_pipeline_layout = *pipeline_layout;

            auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                .layout = guard->neighbor_max_pipeline_layout,
                .compute = RHI::ShaderEntry{.module = guard->neighbor_max_module, .entry_point = "neighborMaxMain", .stage = RHI::ShaderStage::Compute},
                .label = "motion blur neighbor-max pipeline",
            });
            if (!pipeline) {
                cleanup();
                return unexpected(graphics_error_from_rhi(pipeline.error(), "create motion-blur neighbor-max pipeline"));
            }
            guard->neighbor_max_pipeline = *pipeline;
        }

        {
            const slang::ShaderCompileOptions options{
                .targets = shader_compile_targets_for_device(device),
                .entry_points = {slang::ShaderEntryPointRequest{.name = "gatherMain", .stage = slang::ShaderStage::Compute}},
            };
            slang::ShaderVariantCache shader_cache{
                slang::ShaderSource::from_file("Shaders/motion_blur_gather.slang", "motion_blur_gather"),
                options,
                slang::ShaderCompiler{},
                recovery_create_info_.enable_shader_disk_cache};
            auto shader = shader_cache.get_or_compile_base();
            if (!shader) {
                cleanup();
                return unexpected(motion_blur_error("compile motion-blur gather shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
            }
            guard->gather_shader = *shader;

            auto code = guard->gather_shader.entry_point_code("gatherMain", shader_target->slang_target.format);
            if (!code) {
                cleanup();
                return unexpected(motion_blur_error("generate motion-blur gather bytecode failed: " + code.error().message));
            }
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = "motion blur gather compute module",
            });
            if (!module) {
                cleanup();
                return unexpected(graphics_error_from_rhi(module.error(), "create motion-blur gather compute module"));
            }
            guard->gather_module = *module;

            const slang::ShaderReflection &reflection = guard->gather_shader.reflection();
            const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
            if (generated.empty()) {
                cleanup();
                return unexpected(motion_blur_error("motion-blur gather shader reflection produced no bind-group layout."));
            }
            auto layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
                .label = "motion blur gather bind group layout",
            });
            if (!layout) {
                cleanup();
                return unexpected(graphics_error_from_rhi(layout.error(), "create motion-blur gather bind group layout"));
            }
            guard->gather_bind_group_layout = *layout;

            const vector<ReflectedResource> resources = collect_resource_bindings(reflection);
            auto scene_color_binding = find_binding(resources, "sceneColor", "motion-blur gather");
            if (!scene_color_binding) { cleanup(); return unexpected(scene_color_binding.error()); }
            guard->gather_scene_color_binding = *scene_color_binding;
            auto sampler_binding = find_binding(resources, "sceneColorSampler", "motion-blur gather");
            if (!sampler_binding) { cleanup(); return unexpected(sampler_binding.error()); }
            guard->gather_sampler_binding = *sampler_binding;
            auto motion_binding = find_binding(resources, "gbufferMotion", "motion-blur gather");
            if (!motion_binding) { cleanup(); return unexpected(motion_binding.error()); }
            guard->gather_motion_binding = *motion_binding;
            auto depth_binding = find_binding(resources, "gbufferDepth", "motion-blur gather");
            if (!depth_binding) { cleanup(); return unexpected(depth_binding.error()); }
            guard->gather_depth_binding = *depth_binding;
            auto dilated_binding = find_binding(resources, "dilatedVelocity", "motion-blur gather");
            if (!dilated_binding) { cleanup(); return unexpected(dilated_binding.error()); }
            guard->gather_dilated_velocity_binding = *dilated_binding;
            auto output_binding = find_binding(resources, "sceneColorOut", "motion-blur gather");
            if (!output_binding) { cleanup(); return unexpected(output_binding.error()); }
            guard->gather_output_binding = *output_binding;

            const vector<RHI::PushConstantRange> push_constant_ranges =
                generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
            if (push_constant_ranges.empty()) {
                cleanup();
                return unexpected(motion_blur_error("motion-blur gather shader produced no push-constant range."));
            }
            const array<RHI::BindGroupLayoutHandle, 1> layouts{guard->gather_bind_group_layout};
            auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
                .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
                .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
                .label = "motion blur gather pipeline layout",
            });
            if (!pipeline_layout) {
                cleanup();
                return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create motion-blur gather pipeline layout"));
            }
            guard->gather_pipeline_layout = *pipeline_layout;

            auto sampler = device->create_sampler(RHI::SamplerDesc{
                .min_filter = RHI::Filter::Linear,
                .mag_filter = RHI::Filter::Linear,
                .mipmap_mode = RHI::MipmapMode::Nearest,
                .address_u = RHI::AddressMode::ClampToEdge,
                .address_v = RHI::AddressMode::ClampToEdge,
                .address_w = RHI::AddressMode::ClampToEdge,
                .max_lod = 0.0f,
                .label = "motion blur gather scene-color sampler",
            });
            if (!sampler) {
                cleanup();
                return unexpected(graphics_error_from_rhi(sampler.error(), "create motion-blur gather sampler"));
            }
            guard->gather_sampler = *sampler;

            auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
                .layout = guard->gather_pipeline_layout,
                .compute = RHI::ShaderEntry{.module = guard->gather_module, .entry_point = "gatherMain", .stage = RHI::ShaderStage::Compute},
                .label = "motion blur gather pipeline",
            });
            if (!pipeline) {
                cleanup();
                return unexpected(graphics_error_from_rhi(pipeline.error(), "create motion-blur gather pipeline"));
            }
            guard->gather_pipeline = *pipeline;
        }

        guard->tile_max_shader.release_compiler_state();
        guard->neighbor_max_shader.release_compiler_state();
        guard->gather_shader.release_compiler_state();
        guard->ready = true;
        return {};
    }

    /// Records the motion-blur tile-max reduction pass using the supplied arguments and current state.
    ///
    /// @param pass Compute-pass encoder that receives the dispatch.
    /// @param motion_view Full-resolution per-pixel motion vector view produced by the deferred G-buffer pass.
    /// @param tile_max_output_view Tile-resolution destination for the per-tile max-magnitude velocity.
    /// @param render_extent Full render resolution in pixels.
    /// @param tile_size Tile edge length in pixels (`MotionBlurSettings::tile_size_px`).
    /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_motion_blur_tile_max(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle motion_view,
        RHI::TextureViewHandle tile_max_output_view,
        glm::uvec2 render_extent,
        u32 tile_size,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_motion_blur_tile_max");
        if (Core::RendererResult ready = ensure_motion_blur_resources(); !ready.has_value()) {
            return ready;
        }
        if (!motion_view || !tile_max_output_view || render_extent.x == 0 || render_extent.y == 0 || tile_size == 0) {
            return unexpected(motion_blur_error("Cannot record motion-blur tile-max without valid views/extent/tile size."));
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(motion_blur_error("Cannot record motion-blur tile-max without an RHI device."));
        }

        RHI::BindGroupLayoutHandle bind_group_layout{};
        RHI::ComputePipelineHandle pipeline{};
        u32 motion_binding = 0;
        u32 output_binding = 0;
        {
            auto guard = motion_blur_.lock();
            bind_group_layout = guard->tile_max_bind_group_layout;
            pipeline = guard->tile_max_pipeline;
            motion_binding = guard->tile_max_motion_binding;
            output_binding = guard->tile_max_output_binding;
        }
        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = motion_binding, .texture_view = motion_view},
            RHI::BindGroupEntry{.binding = output_binding, .texture_view = tile_max_output_view},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "motion blur tile-max bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create motion-blur tile-max bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        const TileMaxConstants constants{.render_extent = render_extent, .tile_size = tile_size};
        pass.set_push_constants(RHI::ShaderStage::Compute, 0, std::as_bytes(span<const TileMaxConstants>{&constants, 1}));

        const u32 tiles_x = (render_extent.x + tile_size - 1) / tile_size;
        const u32 tiles_y = (render_extent.y + tile_size - 1) / tile_size;
        pass.dispatch(tiles_x, tiles_y, 1);
        return {};
    }

    /// Records the motion-blur neighbor-max dilation pass using the supplied arguments and current state.
    ///
    /// @param pass Compute-pass encoder that receives the dispatch.
    /// @param tile_max_view Tile-resolution per-tile max velocity produced by `record_motion_blur_tile_max`.
    /// @param dilated_output_view Tile-resolution destination for the neighborhood-dilated velocity.
    /// @param tile_extent Tile-grid resolution (`ceil(render_extent / tile_size)`).
    /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_motion_blur_neighbor_max(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle tile_max_view,
        RHI::TextureViewHandle dilated_output_view,
        glm::uvec2 tile_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_motion_blur_neighbor_max");
        if (Core::RendererResult ready = ensure_motion_blur_resources(); !ready.has_value()) {
            return ready;
        }
        if (!tile_max_view || !dilated_output_view || tile_extent.x == 0 || tile_extent.y == 0) {
            return unexpected(motion_blur_error("Cannot record motion-blur neighbor-max without valid views/extent."));
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(motion_blur_error("Cannot record motion-blur neighbor-max without an RHI device."));
        }

        RHI::BindGroupLayoutHandle bind_group_layout{};
        RHI::ComputePipelineHandle pipeline{};
        u32 input_binding = 0;
        u32 output_binding = 0;
        {
            auto guard = motion_blur_.lock();
            bind_group_layout = guard->neighbor_max_bind_group_layout;
            pipeline = guard->neighbor_max_pipeline;
            input_binding = guard->neighbor_max_input_binding;
            output_binding = guard->neighbor_max_output_binding;
        }
        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = input_binding, .texture_view = tile_max_view},
            RHI::BindGroupEntry{.binding = output_binding, .texture_view = dilated_output_view},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "motion blur neighbor-max bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create motion-blur neighbor-max bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        const NeighborMaxConstants constants{.tile_extent = tile_extent};
        pass.set_push_constants(RHI::ShaderStage::Compute, 0, std::as_bytes(span<const NeighborMaxConstants>{&constants, 1}));

        pass.dispatch((tile_extent.x + 7u) / 8u, (tile_extent.y + 7u) / 8u, 1);
        return {};
    }

    /// Records the motion-blur gather (line-integral reconstruction) pass using the supplied arguments and current state.
    ///
    /// @param pass Compute-pass encoder that receives the dispatch.
    /// @param scene_color_view Full-resolution HDR scene color view to blur.
    /// @param motion_view Full-resolution per-pixel motion vector view.
    /// @param depth_view Full-resolution scene depth view, used for the foreground/background tap weighting.
    /// @param dilated_velocity_view Tile-resolution dilated velocity produced by `record_motion_blur_neighbor_max`.
    /// @param output_view Full-resolution destination for the blurred scene color.
    /// @param settings Configuration values controlling the operation.
    /// @param render_extent Full render resolution in pixels.
    /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_motion_blur_gather(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle scene_color_view,
        RHI::TextureViewHandle motion_view,
        RHI::TextureViewHandle depth_view,
        RHI::TextureViewHandle dilated_velocity_view,
        RHI::TextureViewHandle output_view,
        const RenderGraphSettings &settings,
        glm::uvec2 render_extent,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_motion_blur_gather");
        if (Core::RendererResult ready = ensure_motion_blur_resources(); !ready.has_value()) {
            return ready;
        }
        if (!scene_color_view || !motion_view || !depth_view || !dilated_velocity_view || !output_view ||
            render_extent.x == 0 || render_extent.y == 0) {
            return unexpected(motion_blur_error("Cannot record motion-blur gather without valid views/extent."));
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(motion_blur_error("Cannot record motion-blur gather without an RHI device."));
        }

        RHI::BindGroupLayoutHandle bind_group_layout{};
        RHI::ComputePipelineHandle pipeline{};
        RHI::SamplerHandle sampler{};
        u32 scene_color_binding = 0;
        u32 sampler_binding = 0;
        u32 motion_binding = 0;
        u32 depth_binding = 0;
        u32 dilated_binding = 0;
        u32 output_binding = 0;
        {
            auto guard = motion_blur_.lock();
            bind_group_layout = guard->gather_bind_group_layout;
            pipeline = guard->gather_pipeline;
            sampler = guard->gather_sampler;
            scene_color_binding = guard->gather_scene_color_binding;
            sampler_binding = guard->gather_sampler_binding;
            motion_binding = guard->gather_motion_binding;
            depth_binding = guard->gather_depth_binding;
            dilated_binding = guard->gather_dilated_velocity_binding;
            output_binding = guard->gather_output_binding;
        }
        const array<RHI::BindGroupEntry, 6> entries{
            RHI::BindGroupEntry{.binding = scene_color_binding, .texture_view = scene_color_view},
            RHI::BindGroupEntry{.binding = sampler_binding, .sampler = sampler},
            RHI::BindGroupEntry{.binding = motion_binding, .texture_view = motion_view},
            RHI::BindGroupEntry{.binding = depth_binding, .texture_view = depth_view},
            RHI::BindGroupEntry{.binding = dilated_binding, .texture_view = dilated_velocity_view},
            RHI::BindGroupEntry{.binding = output_binding, .texture_view = output_view},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "motion blur gather bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create motion-blur gather bind group"));
        }
        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        const GatherConstants constants{
            .render_extent = glm::vec2{static_cast<f32>(render_extent.x), static_cast<f32>(render_extent.y)},
            .intensity = settings.motion_blur.intensity,
            .shutter_fraction = settings.motion_blur.shutter_angle_degrees / 360.0f,
            .max_blur_radius_px = settings.motion_blur.max_blur_radius_px,
            .fg_bg_weight_bias = settings.motion_blur.background_foreground_weight_bias,
            .sample_count = std::max(settings.motion_blur.sample_count, 1u),
            .tile_size = std::max(settings.motion_blur.tile_size_px, 1u),
            .camera_motion_only = settings.motion_blur.camera_motion_only ? 1u : 0u,
        };
        pass.set_push_constants(RHI::ShaderStage::Compute, 0, std::as_bytes(span<const GatherConstants>{&constants, 1}));

        pass.dispatch((render_extent.x + 7u) / 8u, (render_extent.y + 7u) / 8u, 1);
        return {};
    }

    /// Destroys the motion blur resources identified by the supplied parameters.
    ///
    /// @note This function does not throw exceptions.
    void Renderer::destroy_motion_blur_resources() noexcept {
        ZoneScopedN("Renderer::destroy_motion_blur_resources");
        RHI::RhiDevice *device = rhi_device();
        auto guard = motion_blur_.lock();
        if (device != nullptr) {
            if (guard->gather_sampler) device->destroy_sampler(guard->gather_sampler);
            if (guard->gather_pipeline) device->destroy_compute_pipeline(guard->gather_pipeline);
            if (guard->gather_pipeline_layout) device->destroy_pipeline_layout(guard->gather_pipeline_layout);
            if (guard->gather_bind_group_layout) device->destroy_bind_group_layout(guard->gather_bind_group_layout);
            if (guard->gather_module) device->destroy_shader_module(guard->gather_module);
            if (guard->neighbor_max_pipeline) device->destroy_compute_pipeline(guard->neighbor_max_pipeline);
            if (guard->neighbor_max_pipeline_layout) device->destroy_pipeline_layout(guard->neighbor_max_pipeline_layout);
            if (guard->neighbor_max_bind_group_layout) device->destroy_bind_group_layout(guard->neighbor_max_bind_group_layout);
            if (guard->neighbor_max_module) device->destroy_shader_module(guard->neighbor_max_module);
            if (guard->tile_max_pipeline) device->destroy_compute_pipeline(guard->tile_max_pipeline);
            if (guard->tile_max_pipeline_layout) device->destroy_pipeline_layout(guard->tile_max_pipeline_layout);
            if (guard->tile_max_bind_group_layout) device->destroy_bind_group_layout(guard->tile_max_bind_group_layout);
            if (guard->tile_max_module) device->destroy_shader_module(guard->tile_max_module);
        }
        *guard = {};
    }

} // namespace SFT::Renderer
