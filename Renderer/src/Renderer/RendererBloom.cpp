#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>


#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Core/Core.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Renderer/RendererModule.hpp>

#include <tracy/Tracy.hpp>


using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {
    namespace {
        struct BloomConstants {
            glm::vec2 source_texel_size{1.0f};
            f32 threshold = 1.0f;
            f32 soft_knee = 0.5f;
            glm::vec2 filter_scale{1.0f};
        };
        static_assert(sizeof(BloomConstants) == 24);

        struct BloomCompositeConstants {
            f32 bloom_intensity = 0.0f;
            u32 threshold_enabled = 0;
        };
        static_assert(sizeof(BloomCompositeConstants) == 8);

        /// Creates an error result describing the supplied bloom failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError bloom_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }
    } // namespace

    /// Finds or creates the bloom resources required by the operation.
    ///
    /// @param color_format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_bloom_resources(RHI::Format color_format) {
        ZoneScopedN("Renderer::ensure_bloom_resources");
        auto guard = bloom_.lock();
        if (guard->ready && guard->color_format == color_format) return {};
        if (guard->ready) destroy_bloom_resources_locked(*guard);

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) return unexpected(bloom_error("Cannot build bloom resources without an RHI device."));

        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) return unexpected(shader_target.error());

        const slang::ShaderCompileOptions options{
            .targets = shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "prefilterMain", .stage = slang::ShaderStage::Fragment},
                slang::ShaderEntryPointRequest{.name = "downsampleMain", .stage = slang::ShaderStage::Fragment},
                slang::ShaderEntryPointRequest{.name = "upsampleMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file("Shaders/fullscreen_bloom.slang", "fullscreen_bloom"),
            options,
            slang::ShaderCompiler{},
            recovery_create_info_.enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) return unexpected(bloom_error("compile bloom shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->prefilter_entry_point = "prefilterMain";
        guard->downsample_entry_point = "downsampleMain";
        guard->upsample_entry_point = "upsampleMain";

        auto create_module = [&](string_view entry, const char *label) -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = guard->shader.entry_point_code(entry, shader_target->slang_target.format);
            if (!code) return unexpected(bloom_error("generate bloom shader bytecode failed: " + code.error().message));
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) return unexpected(graphics_error_from_rhi(module.error(), label));
            return *module;
        };
        auto vertex = create_module(guard->vertex_entry_point, "bloom vertex module");
        if (!vertex) return unexpected(vertex.error());
        guard->vertex_module = *vertex;
        auto prefilter = create_module(guard->prefilter_entry_point, "bloom prefilter module");
        if (!prefilter) { destroy_bloom_resources_locked(*guard); return unexpected(prefilter.error()); }
        guard->prefilter_module = *prefilter;
        auto downsample = create_module(guard->downsample_entry_point, "bloom downsample module");
        if (!downsample) { destroy_bloom_resources_locked(*guard); return unexpected(downsample.error()); }
        guard->downsample_module = *downsample;
        auto upsample = create_module(guard->upsample_entry_point, "bloom upsample module");
        if (!upsample) { destroy_bloom_resources_locked(*guard); return unexpected(upsample.error()); }
        guard->upsample_module = *upsample;

        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "bloom bind group layout",
            });
            if (!handle) { destroy_bloom_resources_locked(*guard); return unexpected(graphics_error_from_rhi(handle.error(), "create bloom bind group layout")); }
            guard->bind_group_layouts.push_back(*handle);
            guard->bind_group_layout_sets.push_back(layout.set);
            if (!guard->sampled_layout) {
                bool has_image = false;
                bool has_sampler = false;
                for (const RHI::BindGroupLayoutEntry &entry : layout.entries) {
                    if (entry.type == RHI::BindingType::SampledTexture) {
                        guard->image_binding = entry.binding;
                        has_image = true;
                    } else if (entry.type == RHI::BindingType::Sampler) {
                        guard->sampler_binding = entry.binding;
                        has_sampler = true;
                    }
                }
                if (has_image && has_sampler) {
                    guard->sampled_layout = *handle;
                    guard->sampled_set = layout.set;
                }
            }
        }
        if (!guard->sampled_layout) {
            destroy_bloom_resources_locked(*guard);
            return unexpected(bloom_error("bloom shader reflection produced no sampled texture + sampler layout."));
        }

        const vector<RHI::PushConstantRange> push_ranges = generate_push_constant_ranges(reflection, RHI::ShaderStage::Fragment);
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{guard->bind_group_layouts.data(), guard->bind_group_layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "bloom pipeline layout",
        });
        if (!pipeline_layout) { destroy_bloom_resources_locked(*guard); return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create bloom pipeline layout")); }
        guard->pipeline_layout = *pipeline_layout;

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear, .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge, .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge, .max_lod = 0.0f, .label = "bloom sampler",
        });
        if (!sampler) { destroy_bloom_resources_locked(*guard); return unexpected(graphics_error_from_rhi(sampler.error(), "create bloom sampler")); }
        guard->sampler = *sampler;

        auto create_pipeline = [&](RHI::ShaderModuleHandle fragment, const char *entry, bool additive, const char *label)
            -> Core::RendererExpected<RHI::RenderPipelineHandle> {
            RHI::ColorTargetState target{.format = color_format, .blend_enable = additive, .write_mask = RHI::ColorWriteMask::All};
            if (additive) {


                target.color = RHI::BlendComponent{
                    .src_factor = RHI::BlendFactor::ConstantColor,
                    .dst_factor = RHI::BlendFactor::OneMinusConstantColor,
                    .op = RHI::BlendOp::Add,
                };
                target.alpha = RHI::BlendComponent{
                    .src_factor = RHI::BlendFactor::Zero,
                    .dst_factor = RHI::BlendFactor::One,
                    .op = RHI::BlendOp::Add,
                };
            }
            auto pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
                .layout = guard->pipeline_layout,
                .vertex = RHI::ShaderEntry{.module = guard->vertex_module, .entry_point = guard->vertex_entry_point.c_str(), .stage = RHI::ShaderStage::Vertex},
                .fragment = RHI::ShaderEntry{.module = fragment, .entry_point = entry, .stage = RHI::ShaderStage::Fragment},
                .vertex_buffers = {}, .topology = RHI::PrimitiveTopology::TriangleList,
                .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
                .depth_stencil = RHI::DepthStencilState{},
                .color_targets = span<const RHI::ColorTargetState>{&target, 1}, .label = label,
            });
            if (!pipeline) return unexpected(graphics_error_from_rhi(pipeline.error(), label));
            return *pipeline;
        };
        auto prefilter_pipeline = create_pipeline(guard->prefilter_module, guard->prefilter_entry_point.c_str(), false, "bloom prefilter pipeline");
        if (!prefilter_pipeline) { destroy_bloom_resources_locked(*guard); return unexpected(prefilter_pipeline.error()); }
        guard->prefilter_pipeline = *prefilter_pipeline;
        auto down_pipeline = create_pipeline(guard->downsample_module, guard->downsample_entry_point.c_str(), false, "bloom downsample pipeline");
        if (!down_pipeline) { destroy_bloom_resources_locked(*guard); return unexpected(down_pipeline.error()); }
        guard->downsample_pipeline = *down_pipeline;
        auto up_pipeline = create_pipeline(guard->upsample_module, guard->upsample_entry_point.c_str(), true, "bloom upsample pipeline");
        if (!up_pipeline) { destroy_bloom_resources_locked(*guard); return unexpected(up_pipeline.error()); }
        guard->upsample_pipeline = *up_pipeline;
        guard->color_format = color_format;
        guard->shader.release_compiler_state();
        guard->ready = true;
        return {};
    }

    /// Records bloom draw using the supplied arguments and current state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param source_view `source_view` value used by the operation.
    /// @param source_texel_size Requested or available size for the operation.
    /// @param threshold `threshold` value used by the operation.
    /// @param soft_knee `soft_knee` value used by the operation.
    /// @param scatter `scatter` value used by the operation.
    /// @param filter_scale `filter_scale` value used by the operation.
    /// @param prefilter `prefilter` value used by the operation.
    /// @param upsample `upsample` value used by the operation.
    /// @param bind_group `bind_group` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_bloom_draw(RHI::RenderPassEncoder &pass,
                                                       RHI::TextureViewHandle source_view, glm::vec2 source_texel_size,
                                                       f32 threshold, f32 soft_knee, f32 scatter,
                                                       glm::vec2 filter_scale, bool prefilter, bool upsample,
                                                       RHI::BindGroupHandle bind_group) {
        ZoneScopedN("Renderer::record_bloom_draw");
        RHI::RenderPipelineHandle pipeline{};
        u32 sampled_set = 0;
        {
            auto guard = bloom_.lock();
            if (!source_view || !bind_group || !guard->ready) return unexpected(bloom_error("Cannot record bloom without ready resources, a source texture, and a cached bind group."));
            pipeline = upsample
                ? guard->upsample_pipeline
                : (prefilter ? guard->prefilter_pipeline : guard->downsample_pipeline);
            sampled_set = guard->sampled_set;
        }
        const BloomConstants constants{
            .source_texel_size = source_texel_size,
            .threshold = threshold,
            .soft_knee = soft_knee,
            .filter_scale = filter_scale,
        };
        pass.set_pipeline(pipeline);
        if (upsample) {
            const f32 normalized_scatter = std::clamp(scatter, 0.0f, 1.0f);
            pass.set_blend_constant(RHI::ClearColor{
                normalized_scatter, normalized_scatter, normalized_scatter, normalized_scatter});
        }
        pass.set_bind_group(sampled_set, bind_group);
        pass.set_push_constants(RHI::ShaderStage::Fragment, 0,
                                std::as_bytes(span<const BloomConstants>{&constants, 1}));
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    /// Records bloom downsample using the supplied arguments and current state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param source_view `source_view` value used by the operation.
    /// @param source_texel_size Requested or available size for the operation.
    /// @param settings Configuration values controlling the operation.
    /// @param filter_scale `filter_scale` value used by the operation.
    /// @param apply_threshold `apply_threshold` value used by the operation.
    /// @param bind_group `bind_group` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_bloom_downsample(RHI::RenderPassEncoder &pass, RHI::TextureViewHandle source_view,
                                                            glm::vec2 source_texel_size, const RenderGraphSettings &settings,
                                                            glm::vec2 filter_scale, bool apply_threshold,
                                                            RHI::BindGroupHandle bind_group) {
        ZoneScopedN("Renderer::record_bloom_downsample");
        return record_bloom_draw(pass, source_view, source_texel_size,
                                 settings.bloom_threshold, settings.bloom_soft_knee, settings.bloom_scatter,
                                 filter_scale, apply_threshold, false, bind_group);
    }

    /// Records bloom upsample using the supplied arguments and current state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param source_view `source_view` value used by the operation.
    /// @param source_texel_size Requested or available size for the operation.
    /// @param settings Configuration values controlling the operation.
    /// @param bind_group `bind_group` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_bloom_upsample(RHI::RenderPassEncoder &pass, RHI::TextureViewHandle source_view,
                                                          glm::vec2 source_texel_size, const RenderGraphSettings &settings,
                                                          RHI::BindGroupHandle bind_group) {
        ZoneScopedN("Renderer::record_bloom_upsample");
        return record_bloom_draw(pass, source_view, source_texel_size,
                                 settings.bloom_threshold, settings.bloom_soft_knee, settings.bloom_scatter,
                                 glm::vec2{1.0f}, false, true, bind_group);
    }

    /// Destroys the bloom resources identified by the supplied parameters.
    ///
    /// @return Returns the current destroy bloom resources value.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_bloom_resources() noexcept { auto guard = bloom_.lock(); destroy_bloom_resources_locked(*guard); }

    /// Destroys the bloom resources locked identified by the supplied parameters.
    ///
    /// @param resources `resources` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_bloom_resources_locked(BloomResources &resources) noexcept {
        ZoneScopedN("Renderer::destroy_bloom_resources_locked");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            if (resources.upsample_pipeline) device->destroy_render_pipeline(resources.upsample_pipeline);
            if (resources.downsample_pipeline) device->destroy_render_pipeline(resources.downsample_pipeline);
            if (resources.prefilter_pipeline) device->destroy_render_pipeline(resources.prefilter_pipeline);
            if (resources.sampler) device->destroy_sampler(resources.sampler);
            if (resources.pipeline_layout) device->destroy_pipeline_layout(resources.pipeline_layout);
            for (RHI::BindGroupLayoutHandle layout : resources.bind_group_layouts) device->destroy_bind_group_layout(layout);
            if (resources.upsample_module) device->destroy_shader_module(resources.upsample_module);
            if (resources.downsample_module) device->destroy_shader_module(resources.downsample_module);
            if (resources.prefilter_module) device->destroy_shader_module(resources.prefilter_module);
            if (resources.vertex_module) device->destroy_shader_module(resources.vertex_module);
        }
        resources = {};
    }

    /// Creates a bloom source bind group from the supplied parameters.
    ///
    /// @param source_view `source_view` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RHI::BindGroupHandle> Renderer::create_bloom_source_bind_group(RHI::TextureViewHandle source_view) {
        ZoneScopedN("Renderer::create_bloom_source_bind_group");
        auto guard = bloom_.lock();
        if (!guard->ready || !guard->sampled_layout) {
            return unexpected(bloom_error("Cannot create a bloom source bind group before bloom resources are ready."));
        }
        if (!source_view) {
            return unexpected(bloom_error("Cannot create a bloom source bind group without a source view."));
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) return unexpected(bloom_error("Cannot create a bloom source bind group without an RHI device."));
        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = guard->image_binding, .texture_view = source_view},
            RHI::BindGroupEntry{.binding = guard->sampler_binding, .sampler = guard->sampler},
        };
        auto group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = guard->sampled_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "transient bloom source bind group",
        });
        if (!group) return unexpected(graphics_error_from_rhi(group.error(), "create transient bloom source bind group"));
        return *group;
    }

    /// Adds render-graph nodes that bloom `source` for `Renderer` using the supplied arguments — see
    /// the declaration's own doc comment (RendererModule.hpp) for the full contract.
    ///
    /// @param graph The current frame's render graph.
    /// @param source The element's own already-rendered color texture (with alpha) to bloom.
    /// @param source_extent `source`'s pixel size.
    /// @param output Destination for the final composite; caller-provided (typically an imported
    ///        texture) rather than allocated here.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param settings Configuration values controlling the operation.
    /// @param out_transient_bind_groups Bind group used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::add_ui_glow_bloom_passes(
        RenderGraph &graph, RenderGraphTextureHandle source, Core::Extent2D source_extent,
        RenderGraphTextureHandle output, RHI::Format format, const RenderGraphSettings &settings,
        vector<RHI::BindGroupHandle> &out_transient_bind_groups) {
        ZoneScopedN("Renderer::add_ui_glow_bloom_passes");
        if (Core::RendererResult ready = ensure_bloom_resources(format); !ready) {
            return unexpected(ready.error());
        }

        constexpr usize level_count = 3;
        array<Core::Extent2D, level_count> level_extents{};
        Core::Extent2D previous = source_extent;
        for (usize i = 0; i < level_count; ++i) {
            previous = Core::Extent2D{std::max(previous.x / 2u, 1u), std::max(previous.y / 2u, 1u)};
            level_extents[i] = previous;
        }

        array<RenderGraphTextureHandle, level_count> levels{};
        for (usize level = 0; level < level_count; ++level) {
            levels[level] = graph.create_texture(RenderGraphTextureDesc{
                .format = format,
                .extent = RHI::Extent3D{.width = level_extents[level].x, .height = level_extents[level].y, .depth_or_layers = 1},
                .mip_levels = 1,
                .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
                .initial_layout = RHI::TextureLayout::Undefined,
                .initial_stage = RHI::PipelineStage::None,
                .initial_access = RHI::AccessFlags::None,
            });
        }

        // Downsample chain: level 0 <- source (threshold applied), level 1 <- level 0, level 2 <- level 1.
        // Mirrors Renderer::build_bloom_module's own downsample loop (RendererRenderGraphModules.cpp),
        // generalized to a small fixed level count instead of the main scene's dynamically-sized chain.
        for (usize level = 0; level < level_count; ++level) {
            const RenderGraphTextureHandle pass_source = level == 0 ? source : levels[level - 1];
            const Core::Extent2D src_extent = level == 0 ? source_extent : level_extents[level - 1];
            const Core::Extent2D dst_extent = level_extents[level];
            const bool apply_threshold = level == 0;

            graph.add_render_pass("ui glow bloom downsample"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = levels[level],
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = pass_source})
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = dst_extent.x, .height = dst_extent.y})
                .set_execute([this, pass_source, src_extent, dst_extent, settings, apply_threshold,
                             &out_transient_bind_groups](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(dst_extent.x), .height = static_cast<f32>(dst_extent.y),
                        .min_depth = 0.0f, .max_depth = 1.0f});
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = dst_extent.x, .height = dst_extent.y});
                    const RHI::TextureViewHandle source_view = context.texture(pass_source).default_view;
                    auto bind_group = create_bloom_source_bind_group(source_view);
                    if (!bind_group.has_value()) {
                        return unexpected(bind_group.error());
                    }
                    {
                        auto lock_guard = transient_bind_groups_lock_.lock();
                        out_transient_bind_groups.push_back(*bind_group);
                    }
                    return record_bloom_downsample(
                        pass, source_view,
                        glm::vec2{1.0f / static_cast<f32>(src_extent.x), 1.0f / static_cast<f32>(src_extent.y)},
                        settings,
                        glm::vec2{0.5f * static_cast<f32>(src_extent.x) / static_cast<f32>(dst_extent.x),
                                 0.5f * static_cast<f32>(src_extent.y) / static_cast<f32>(dst_extent.y)},
                        apply_threshold, *bind_group);
                });
        }

        // Upsample chain: level 1 -> blended into level 0's texture, i.e. level_count-1 down to 1.
        for (usize level = level_count; level-- > 1;) {
            const Core::Extent2D src_extent = level_extents[level];
            const Core::Extent2D dst_extent = level_extents[level - 1];
            const RenderGraphTextureHandle pass_source = levels[level];
            const RenderGraphTextureHandle pass_destination = levels[level - 1];

            graph.add_render_pass("ui glow bloom upsample"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = pass_destination,
                    .load_op = RHI::LoadOp::Load,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = pass_source})
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = dst_extent.x, .height = dst_extent.y})
                .set_execute([this, pass_source, src_extent, dst_extent, settings,
                             &out_transient_bind_groups](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{
                        .width = static_cast<f32>(dst_extent.x), .height = static_cast<f32>(dst_extent.y),
                        .min_depth = 0.0f, .max_depth = 1.0f});
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = dst_extent.x, .height = dst_extent.y});
                    const RHI::TextureViewHandle source_view = context.texture(pass_source).default_view;
                    auto bind_group = create_bloom_source_bind_group(source_view);
                    if (!bind_group.has_value()) {
                        return unexpected(bind_group.error());
                    }
                    {
                        auto lock_guard = transient_bind_groups_lock_.lock();
                        out_transient_bind_groups.push_back(*bind_group);
                    }
                    return record_bloom_upsample(
                        pass, source_view,
                        glm::vec2{1.0f / static_cast<f32>(src_extent.x), 1.0f / static_cast<f32>(src_extent.y)},
                        settings, *bind_group);
                });
        }

        // Final composite: blend the now-blurred levels[0] back onto the original `source`, into the
        // caller-provided `output` texture.
        const RenderGraphTextureHandle bloom_level0 = levels[0];
        graph.add_render_pass("ui glow bloom composite"_ustr)
            .add_color_attachment(RenderGraphColorAttachmentDesc{
                .texture = output,
                .load_op = RHI::LoadOp::DontCare,
                .store_op = RHI::StoreOp::Store,
            })
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = source})
            .add_sampled_texture(RenderGraphSampledTextureReadDesc{.texture = bloom_level0})
            .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = source_extent.x, .height = source_extent.y})
            .set_execute([this, source, bloom_level0, source_extent, format, settings,
                         &out_transient_bind_groups](RenderGraphContext &context) -> Core::RendererResult {
                RHI::RenderPassEncoder &pass = context.render_pass();
                pass.set_viewport(RHI::Viewport{
                    .width = static_cast<f32>(source_extent.x), .height = static_cast<f32>(source_extent.y),
                    .min_depth = 0.0f, .max_depth = 1.0f});
                pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = source_extent.x, .height = source_extent.y});
                // threshold_enabled = true selects the additive scene + bloom*intensity branch
                // (fullscreen_bloom_composite.slang) instead of the main scene's own lerp-toward-bloom
                // blend — a UI glow needs to keep its crisp base line and ADD a halo on top of it, not
                // fade the line out as intensity rises.
                return record_bloom_composite(
                    pass, context.texture(source).default_view, context.texture(bloom_level0).default_view,
                    format, settings.bloom_intensity, true, out_transient_bind_groups);
            });

        return {};
    }

    /// Finds or creates the bloom composite resources required by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_bloom_composite_resources() {
        ZoneScopedN("Renderer::ensure_bloom_composite_resources");
        auto guard = bloom_composite_.lock();
        if (guard->ready) return {};

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) return unexpected(bloom_error("Cannot build bloom composite resources without an RHI device."));

        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) return unexpected(shader_target.error());

        const slang::ShaderCompileOptions options{
            .targets = shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file("Shaders/fullscreen_bloom_composite.slang", "fullscreen_bloom_composite"),
            options,
            slang::ShaderCompiler{},
            recovery_create_info_.enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) return unexpected(bloom_error("compile bloom composite shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->fragment_entry_point = "fragmentMain";

        auto vertex_code = guard->shader.entry_point_code(guard->vertex_entry_point, shader_target->slang_target.format);
        if (!vertex_code) return unexpected(bloom_error("generate bloom composite vertex bytecode failed: " + vertex_code.error().message));
        auto vertex_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "bloom composite vertex module",
        });
        if (!vertex_module) return unexpected(graphics_error_from_rhi(vertex_module.error(), "create bloom composite vertex module"));
        guard->vertex_module = *vertex_module;

        auto fragment_code = guard->shader.entry_point_code(guard->fragment_entry_point, shader_target->slang_target.format);
        if (!fragment_code) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(bloom_error("generate bloom composite fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "bloom composite fragment module",
        });
        if (!fragment_module) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(fragment_module.error(), "create bloom composite fragment module"));
        }
        guard->fragment_module = *fragment_module;

        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "bloom composite bind group layout",
            });
            if (!handle) {
                destroy_bloom_composite_resources_locked(*guard);
                return unexpected(graphics_error_from_rhi(handle.error(), "create bloom composite bind group layout"));
            }
            guard->bind_group_layouts.push_back(*handle);
            guard->bind_group_layout_sets.push_back(layout.set);
        }
        if (guard->bind_group_layouts.empty()) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(bloom_error("bloom composite shader produced no bind-group layout (expected two sampled textures + one sampler)."));
        }


        bool has_scene_binding = false;
        bool has_bloom_binding = false;
        bool has_sampler_binding = false;
        for (const GeneratedBindGroupLayout &layout : generated) {
            for (const RHI::BindGroupLayoutEntry &entry : layout.entries) {
                if (entry.type == RHI::BindingType::SampledTexture) {
                    if (!has_scene_binding) { guard->scene_binding = entry.binding; has_scene_binding = true; }
                    else if (!has_bloom_binding) { guard->bloom_binding = entry.binding; has_bloom_binding = true; }
                } else if (entry.type == RHI::BindingType::Sampler && !has_sampler_binding) {
                    guard->sampler_binding = entry.binding;
                    has_sampler_binding = true;
                }
            }
        }
        if (!has_scene_binding || !has_bloom_binding || !has_sampler_binding) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(bloom_error("bloom composite shader reflection did not produce two sampled textures and one sampler."));
        }

        const vector<RHI::PushConstantRange> push_constant_ranges = generate_push_constant_ranges(reflection, RHI::ShaderStage::Fragment);
        if (push_constant_ranges.empty()) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(bloom_error("bloom composite shader produced no push-constant range."));
        }
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{guard->bind_group_layouts.data(), guard->bind_group_layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
            .label = "bloom composite pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create bloom composite pipeline layout"));
        }
        guard->pipeline_layout = *pipeline_layout;

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear, .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge, .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge, .max_lod = 0.0f,
            .label = "bloom composite sampler",
        });
        if (!sampler) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(sampler.error(), "create bloom composite sampler"));
        }
        guard->sampler = *sampler;

        guard->shader.release_compiler_state();
        guard->ready = true;
        return {};
    }

    /// Resolves the bloom composite pipeline associated with the supplied key, handle, or resource.
    ///
    /// @param color_format Format used for the resource, render target, or conversion.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::bloom_composite_pipeline_for(RHI::Format color_format) {
        ZoneScopedN("Renderer::bloom_composite_pipeline_for");
        if (Core::RendererResult ready = ensure_bloom_composite_resources(); !ready) return unexpected(ready.error());

        auto guard = bloom_composite_.lock();
        for (const BloomCompositePipelineVariant &variant : guard->pipeline_variants) {
            if (variant.color_format == color_format) return variant.pipeline;
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) return unexpected(bloom_error("Cannot build a bloom composite pipeline without an RHI device."));

        const RHI::ColorTargetState color_target{.format = color_format, .blend_enable = false, .write_mask = RHI::ColorWriteMask::All};
        auto pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = guard->pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module, .entry_point = guard->vertex_entry_point.c_str(), .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = guard->fragment_module, .entry_point = guard->fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {}, .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "bloom composite pipeline",
        });
        if (!pipeline) return unexpected(graphics_error_from_rhi(pipeline.error(), "create bloom composite pipeline"));
        guard->pipeline_variants.push_back(BloomCompositePipelineVariant{.color_format = color_format, .pipeline = *pipeline});
        return *pipeline;
    }

    /// Records bloom composite using the supplied arguments and current state.
    ///
    /// @param pass Render-pass encoder that receives the draw commands.
    /// @param scene_view `scene_view` value used by the operation.
    /// @param bloom_view `bloom_view` value used by the operation.
    /// @param color_format Format used for the resource, render target, or conversion.
    /// @param bloom_intensity `bloom_intensity` value used by the operation.
    /// @param threshold_enabled `threshold_enabled` value used by the operation.
    /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_bloom_composite(RHI::RenderPassEncoder &pass,
                                                           RHI::TextureViewHandle scene_view,
                                                           RHI::TextureViewHandle bloom_view,
                                                           RHI::Format color_format,
                                                           f32 bloom_intensity,
                                                           bool threshold_enabled,
                                                           vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_bloom_composite");
        auto pipeline = bloom_composite_pipeline_for(color_format);
        if (!pipeline) return unexpected(pipeline.error());
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !scene_view || !bloom_view) {
            return unexpected(bloom_error("Cannot record the bloom composite pass without a device, scene texture, and bloom texture."));
        }

        RHI::BindGroupLayoutHandle bind_group_layout{};
        u32 bind_group_layout_set = 0;
        u32 scene_binding = 0;
        u32 bloom_binding = 0;
        u32 sampler_binding = 0;
        RHI::SamplerHandle sampler{};
        {
            auto guard = bloom_composite_.lock();
            bind_group_layout = guard->bind_group_layouts.front();
            bind_group_layout_set = guard->bind_group_layout_sets.front();
            scene_binding = guard->scene_binding;
            bloom_binding = guard->bloom_binding;
            sampler_binding = guard->sampler_binding;
            sampler = guard->sampler;
        }
        const array<RHI::BindGroupEntry, 3> entries{
            RHI::BindGroupEntry{.binding = scene_binding, .texture_view = scene_view},
            RHI::BindGroupEntry{.binding = bloom_binding, .texture_view = bloom_view},
            RHI::BindGroupEntry{.binding = sampler_binding, .sampler = sampler},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "bloom composite bind group",
        });
        if (!bind_group) return unexpected(graphics_error_from_rhi(bind_group.error(), "create bloom composite bind group"));


        { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

        pass.set_pipeline(*pipeline);
        pass.set_bind_group(bind_group_layout_set, *bind_group);
        const BloomCompositeConstants constants{
            .bloom_intensity = bloom_intensity,
            .threshold_enabled = threshold_enabled ? 1u : 0u,
        };
        pass.set_push_constants(RHI::ShaderStage::Fragment, 0,
                                std::as_bytes(span<const BloomCompositeConstants>{&constants, 1}));
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    /// Destroys the bloom composite resources identified by the supplied parameters.
    ///
    /// @return Returns the current destroy bloom composite resources value.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_bloom_composite_resources() noexcept {
        ZoneScopedN("Renderer::destroy_bloom_composite_resources");
        auto guard = bloom_composite_.lock();
        destroy_bloom_composite_resources_locked(*guard);
    }

    /// Destroys the bloom composite resources locked identified by the supplied parameters.
    ///
    /// @param resources `resources` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_bloom_composite_resources_locked(BloomCompositeResources &resources) noexcept {
        ZoneScopedN("Renderer::destroy_bloom_composite_resources_locked");
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (const BloomCompositePipelineVariant &variant : resources.pipeline_variants) {
                if (variant.pipeline) device->destroy_render_pipeline(variant.pipeline);
            }
            if (resources.sampler) device->destroy_sampler(resources.sampler);
            if (resources.pipeline_layout) device->destroy_pipeline_layout(resources.pipeline_layout);
            for (RHI::BindGroupLayoutHandle layout : resources.bind_group_layouts) device->destroy_bind_group_layout(layout);
            if (resources.fragment_module) device->destroy_shader_module(resources.fragment_module);
            if (resources.vertex_module) device->destroy_shader_module(resources.vertex_module);
        }
        resources = {};
    }
} // namespace SFT::Renderer
