#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#pragma region Imports
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

#include <glm/common.hpp>

#include <tracy/Tracy.hpp>

using std::array;
using std::span;
using std::string_view;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        /// Creates an error result describing the supplied hiz failure.
        ///
        /// @param message Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] Core::GraphicsBackendError hiz_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }


        /// Performs the next mip extent operation for `Renderer` using the supplied arguments.
        ///
        /// @param extent `extent` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] Core::Extent2D next_mip_extent(Core::Extent2D extent) noexcept {
            return glm::max(extent / 2u, Core::Extent2D{1u, 1u});
        }

    } // namespace

    /// Finds or creates the hiz build resources required by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_hiz_build_resources() {
        ZoneScopedN("Renderer::ensure_hiz_build_resources");
        auto guard = hiz_build_.lock();
        if (guard->ready) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(hiz_error("Cannot build Hi-Z resources without an RHI device."));
        }

        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) return unexpected(shader_target.error());

        const slang::ShaderCompileOptions options{
            .targets = shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "reduceMain", .stage = slang::ShaderStage::Fragment},
            },
        };


        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file("Shaders/hiz_build.slang", "hiz_build"),
            options,
            slang::ShaderCompiler{},
            recovery_create_info_.enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) {
            return unexpected(hiz_error("compile Hi-Z build shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }
        guard->shader = *shader;

        auto create_module = [&](string_view entry, const char *label) -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = guard->shader.entry_point_code(entry, shader_target->slang_target.format);
            if (!code) return unexpected(hiz_error("generate Hi-Z build bytecode failed: " + code.error().message));
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) return unexpected(graphics_error_from_rhi(module.error(), label));
            return *module;
        };
        auto vertex = create_module("vertexMain", "hiz build vertex module");
        if (!vertex) return unexpected(vertex.error());
        guard->vertex_module = *vertex;
        auto reduce = create_module("reduceMain", "hiz build reduce module");
        if (!reduce) { destroy_hiz_build_resources(); return unexpected(reduce.error()); }
        guard->reduce_module = *reduce;

        // Separate file (hiz_build_from_depth.slang) and compile+link for the depth-source variant:
        // Slang's parameter reflection enumerates every module-scope global regardless of which
        // entry point references it, so a `depthSource` declared alongside `source` in the same
        // module would show up in both variants' bind-group layouts even though each entry point
        // only samples one of them. Keeping them in separate files/modules is what actually gets
        // each variant a bind-group layout with exactly the one SampledTexture entry it uses. See
        // hiz_build_from_depth.slang for the full rationale.
        const slang::ShaderCompileOptions depth_options{
            .targets = shader_compile_targets_for_device(device),
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "reduceFromDepthMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderVariantCache depth_shader_cache{
            slang::ShaderSource::from_file("Shaders/hiz_build_from_depth.slang", "hiz_build_from_depth"),
            depth_options,
            slang::ShaderCompiler{},
            recovery_create_info_.enable_shader_disk_cache};
        auto depth_shader = depth_shader_cache.get_or_compile_base();
        if (!depth_shader) {
            destroy_hiz_build_resources();
            return unexpected(hiz_error("compile Hi-Z depth-source build shader failed: " + depth_shader.error().message + "\n" + depth_shader.error().diagnostics));
        }
        guard->depth_shader = *depth_shader;

        auto create_depth_module = [&](string_view entry, const char *label) -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = guard->depth_shader.entry_point_code(entry, shader_target->slang_target.format);
            if (!code) return unexpected(hiz_error("generate Hi-Z depth-source build bytecode failed: " + code.error().message));
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = shader_target->module_language,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) return unexpected(graphics_error_from_rhi(module.error(), label));
            return *module;
        };
        auto depth_reduce = create_depth_module("reduceFromDepthMain", "hiz build depth-source reduce module");
        if (!depth_reduce) { destroy_hiz_build_resources(); return unexpected(depth_reduce.error()); }
        guard->depth_reduce_module = *depth_reduce;

        const slang::ShaderReflection &depth_reflection = guard->depth_shader.reflection();
        const vector<GeneratedBindGroupLayout> depth_generated = generate_bind_group_layouts(depth_reflection, reflected_stage_mask(depth_reflection));
        if (depth_generated.empty()) {
            destroy_hiz_build_resources();
            return unexpected(hiz_error("Hi-Z depth-source build shader reflection produced no bind-group layout."));
        }
        auto depth_handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{depth_generated.front().entries.data(), depth_generated.front().entries.size()},
            .label = "hiz build depth-source bind group layout",
        });
        if (!depth_handle) { destroy_hiz_build_resources(); return unexpected(graphics_error_from_rhi(depth_handle.error(), "create hiz build depth-source bind group layout")); }
        guard->depth_bind_group_layout = *depth_handle;
        for (const RHI::BindGroupLayoutEntry &entry : depth_generated.front().entries) {
            if (entry.type == RHI::BindingType::SampledTexture) {
                guard->depth_source_binding = entry.binding;
                break;
            }
        }

        const vector<RHI::PushConstantRange> depth_push_ranges = generate_push_constant_ranges(depth_reflection, RHI::ShaderStage::Fragment);
        const array<RHI::BindGroupLayoutHandle, 1> depth_layouts{guard->depth_bind_group_layout};
        auto depth_pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{depth_layouts.data(), depth_layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{depth_push_ranges.data(), depth_push_ranges.size()},
            .label = "hiz build depth-source pipeline layout",
        });
        if (!depth_pipeline_layout) { destroy_hiz_build_resources(); return unexpected(graphics_error_from_rhi(depth_pipeline_layout.error(), "create hiz build depth-source pipeline layout")); }
        guard->depth_pipeline_layout = *depth_pipeline_layout;

        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        if (generated.empty()) {
            destroy_hiz_build_resources();
            return unexpected(hiz_error("Hi-Z build shader reflection produced no bind-group layout."));
        }
        auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
            .label = "hiz build bind group layout",
        });
        if (!handle) { destroy_hiz_build_resources(); return unexpected(graphics_error_from_rhi(handle.error(), "create hiz build bind group layout")); }
        guard->bind_group_layout = *handle;
        for (const RHI::BindGroupLayoutEntry &entry : generated.front().entries) {
            if (entry.type == RHI::BindingType::SampledTexture) {
                guard->source_binding = entry.binding;
                break;
            }
        }

        const vector<RHI::PushConstantRange> push_ranges = generate_push_constant_ranges(reflection, RHI::ShaderStage::Fragment);
        const array<RHI::BindGroupLayoutHandle, 1> layouts{guard->bind_group_layout};
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{layouts.data(), layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "hiz build pipeline layout",
        });
        if (!pipeline_layout) { destroy_hiz_build_resources(); return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create hiz build pipeline layout")); }
        guard->pipeline_layout = *pipeline_layout;

        const RHI::ColorTargetState target{.format = guard->color_format, .blend_enable = false, .write_mask = RHI::ColorWriteMask::All};
        auto pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = guard->pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = guard->reduce_module, .entry_point = "reduceMain", .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&target, 1},
            .label = "hiz build pipeline",
        });
        if (!pipeline) { destroy_hiz_build_resources(); return unexpected(graphics_error_from_rhi(pipeline.error(), "create hiz build pipeline")); }
        guard->pipeline = *pipeline;

        auto depth_pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = guard->depth_pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = guard->depth_reduce_module, .entry_point = "reduceFromDepthMain", .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&target, 1},
            .label = "hiz build depth-source pipeline",
        });
        if (!depth_pipeline) { destroy_hiz_build_resources(); return unexpected(graphics_error_from_rhi(depth_pipeline.error(), "create hiz build depth-source pipeline")); }
        guard->depth_pipeline = *depth_pipeline;

        guard->shader.release_compiler_state();
        guard->depth_shader.release_compiler_state();
        guard->ready = true;
        return {};
    }

    /// Destroys the hiz build resources identified by the supplied parameters.
    ///
    /// @return Returns the current destroy hiz build resources value.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_hiz_build_resources() noexcept {
        ZoneScopedN("Renderer::destroy_hiz_build_resources");
        auto guard = hiz_build_.lock();
        if (RHI::RhiDevice *device = rhi_device()) {
            if (guard->pipeline) device->destroy_render_pipeline(guard->pipeline);
            if (guard->depth_pipeline) device->destroy_render_pipeline(guard->depth_pipeline);
            if (guard->pipeline_layout) device->destroy_pipeline_layout(guard->pipeline_layout);
            if (guard->depth_pipeline_layout) device->destroy_pipeline_layout(guard->depth_pipeline_layout);
            if (guard->bind_group_layout) device->destroy_bind_group_layout(guard->bind_group_layout);
            if (guard->depth_bind_group_layout) device->destroy_bind_group_layout(guard->depth_bind_group_layout);
            if (guard->reduce_module) device->destroy_shader_module(guard->reduce_module);
            if (guard->depth_reduce_module) device->destroy_shader_module(guard->depth_reduce_module);
            if (guard->vertex_module) device->destroy_shader_module(guard->vertex_module);
        }
        *guard = HiZBuildResources{};
    }

    /// Finds or creates the hiz pyramid required by the operation.
    ///
    /// @param pyramid `pyramid` value used by the operation.
    /// @param depth_extent `depth_extent` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::ensure_hiz_pyramid(HiZPyramidTargets &pyramid, Core::Extent2D depth_extent) {
        ZoneScopedN("Renderer::ensure_hiz_pyramid");
        if (Core::is_zero(depth_extent)) {
            return unexpected(hiz_error("Cannot build a Hi-Z pyramid for a zero-sized depth extent."));
        }


        const Core::Extent2D base_extent = next_mip_extent(depth_extent);
        const bool matches = pyramid.extent == base_extent &&
            pyramid.texture && !pyramid.mip_views.empty() && pyramid.full_view;
        if (matches) {
            return {};
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(hiz_error("Cannot build a Hi-Z pyramid without an RHI device."));
        }
        destroy_hiz_pyramid(pyramid);
        pyramid.extent = base_extent;

        vector<Core::Extent2D> level_extents;
        Core::Extent2D level_extent = base_extent;
        for (;;) {
            level_extents.push_back(level_extent);
            if (level_extent.x == 1u && level_extent.y == 1u) break;
            level_extent = next_mip_extent(level_extent);
        }
        pyramid.mip_levels = static_cast<u32>(level_extents.size());

        auto texture = device->create_texture(RHI::TextureDesc{
            .dimension = RHI::TextureDimension::Dim2D,
            .format = RHI::Format::R32Float,
            .extent = RHI::Extent3D{.width = level_extents.front().x, .height = level_extents.front().y, .depth_or_layers = 1},
            .mip_levels = pyramid.mip_levels,
            .samples = RHI::SampleCount::X1,
            .usage = RHI::TextureUsage::ColorAttachment | RHI::TextureUsage::Sampled,
            .label = "persistent hi-z pyramid",
        });
        if (!texture) {
            destroy_hiz_pyramid(pyramid);
            return unexpected(graphics_error_from_rhi(texture.error(), "create persistent hi-z pyramid"));
        }
        pyramid.texture = *texture;

        for (u32 level = 0; level < pyramid.mip_levels; ++level) {
            auto view = device->create_texture_view(RHI::TextureViewDesc{
                .texture = *texture,
                .view_type = RHI::TextureViewType::View2D,
                .base_mip_level = level,
                .mip_level_count = 1,
                .label = "hi-z pyramid mip view",
            });
            if (!view) {
                destroy_hiz_pyramid(pyramid);
                return unexpected(graphics_error_from_rhi(view.error(), "create hi-z pyramid mip view"));
            }
            pyramid.mip_views.push_back(*view);
        }

        auto full_view = device->create_texture_view(RHI::TextureViewDesc{
            .texture = *texture,
            .view_type = RHI::TextureViewType::View2D,
            .base_mip_level = 0,
            .mip_level_count = RHI::all_remaining,
            .label = "hi-z pyramid full view",
        });
        if (!full_view) {
            destroy_hiz_pyramid(pyramid);
            return unexpected(graphics_error_from_rhi(full_view.error(), "create hi-z pyramid full view"));
        }
        pyramid.full_view = *full_view;


        pyramid.has_valid_data = false;
        return {};
    }

    /// Destroys the hiz pyramid identified by the supplied parameters.
    ///
    /// @param pyramid `pyramid` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Renderer::destroy_hiz_pyramid(HiZPyramidTargets &pyramid) noexcept {
        ZoneScopedN("Renderer::destroy_hiz_pyramid");
        if (RHI::RhiDevice *device = rhi_device()) {
            if (pyramid.full_view) device->destroy_texture_view(pyramid.full_view);
            for (RHI::TextureViewHandle view : pyramid.mip_views) {
                if (view) device->destroy_texture_view(view);
            }
            if (pyramid.texture) device->destroy_texture(pyramid.texture);
        }
        pyramid = HiZPyramidTargets{};
    }

    /// Records hiz build using the supplied arguments and current state.
    ///
    /// @param graph `graph` value used by the operation.
    /// @param depth_texture Texture used or affected by the operation.
    /// @param depth_view `depth_view` value used by the operation.
    /// @param depth_extent `depth_extent` value used by the operation.
    /// @param pyramid_texture Texture used or affected by the operation.
    /// @param pyramid `pyramid` value used by the operation.
    /// @param transient_bind_groups `transient_bind_groups` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    Core::RendererResult Renderer::record_hiz_build(RenderGraph &graph, RenderGraphTextureHandle depth_texture,
                                                     RHI::TextureViewHandle depth_view, Core::Extent2D depth_extent,
                                                     RenderGraphTextureHandle pyramid_texture, HiZPyramidTargets &pyramid,
                                                     vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_hiz_build");
        if (pyramid.mip_levels == 0 || pyramid.mip_views.empty()) {
            return unexpected(hiz_error("Cannot record Hi-Z build passes without a prepared pyramid."));
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(hiz_error("Cannot record Hi-Z build passes without an RHI device."));
        }

        RHI::BindGroupLayoutHandle bind_group_layout{};
        RHI::BindGroupLayoutHandle depth_bind_group_layout{};
        u32 source_binding = 0;
        u32 depth_source_binding = 0;
        RHI::RenderPipelineHandle pipeline{};
        RHI::RenderPipelineHandle depth_pipeline{};
        {
            auto guard = hiz_build_.lock();
            bind_group_layout = guard->bind_group_layout;
            depth_bind_group_layout = guard->depth_bind_group_layout;
            source_binding = guard->source_binding;
            depth_source_binding = guard->depth_source_binding;
            pipeline = guard->pipeline;
            depth_pipeline = guard->depth_pipeline;
        }


        Core::Extent2D source_extent = depth_extent;
        for (u32 level = 0; level < pyramid.mip_levels; ++level) {
            const Core::Extent2D destination_extent = next_mip_extent(source_extent);
            const RHI::TextureViewHandle destination_view = pyramid.mip_views[level];
            const bool from_real_depth = level == 0;
            const RHI::TextureViewHandle source_view_for_bind = from_real_depth ? depth_view : pyramid.mip_views[level - 1];
            const Core::Extent2D this_source_extent = source_extent;
            const RHI::BindGroupLayoutHandle level_bind_group_layout = from_real_depth ? depth_bind_group_layout : bind_group_layout;
            const u32 level_source_binding = from_real_depth ? depth_source_binding : source_binding;
            const RHI::RenderPipelineHandle level_pipeline = from_real_depth ? depth_pipeline : pipeline;

            graph.add_render_pass("hiz build"_ustr)
                .add_color_attachment(RenderGraphColorAttachmentDesc{
                    .texture = pyramid_texture,
                    .view = destination_view,


                    .subresources = RHI::TextureSubresourceRange{.base_mip_level = level, .mip_level_count = 1},
                    .load_op = RHI::LoadOp::DontCare,
                    .store_op = RHI::StoreOp::Store,
                })
                .add_sampled_texture(RenderGraphSampledTextureReadDesc{
                    .texture = from_real_depth ? depth_texture : pyramid_texture,
                    .subresources = from_real_depth
                        ? RHI::TextureSubresourceRange{}
                        : RHI::TextureSubresourceRange{.base_mip_level = level - 1, .mip_level_count = 1},
                })
                .set_render_area(RHI::Rect2D{.x = 0, .y = 0, .width = destination_extent.x, .height = destination_extent.y})
                .set_execute([this, level_bind_group_layout, level_source_binding, level_pipeline, source_view_for_bind,
                              this_source_extent, destination_extent, &transient_bind_groups](RenderGraphContext &context) -> Core::RendererResult {
                    RHI::RhiDevice *device = rhi_device();
                    if (device == nullptr) return unexpected(hiz_error("Cannot record hiz build without an RHI device."));
                    const array<RHI::BindGroupEntry, 1> entries{
                        RHI::BindGroupEntry{.binding = level_source_binding, .texture_view = source_view_for_bind},
                    };
                    auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
                        .layout = level_bind_group_layout,
                        .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
                        .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "hiz build bind group",
                    });
                    if (!bind_group) return unexpected(graphics_error_from_rhi(bind_group.error(), "create hiz build bind group"));


                    { auto tbg_guard = transient_bind_groups_lock_.lock(); transient_bind_groups.push_back(*bind_group); }

                    RHI::RenderPassEncoder &pass = context.render_pass();
                    pass.set_viewport(RHI::Viewport{.width = static_cast<f32>(destination_extent.x), .height = static_cast<f32>(destination_extent.y), .min_depth = 0.0f, .max_depth = 1.0f});
                    pass.set_scissor(RHI::Rect2D{.x = 0, .y = 0, .width = destination_extent.x, .height = destination_extent.y});
                    pass.set_pipeline(level_pipeline);
                    pass.set_bind_group(0, *bind_group);
                    struct HiZBuildConstants { u32 source_extent[2]; };
                    const HiZBuildConstants constants{.source_extent = {this_source_extent.x, this_source_extent.y}};
                    pass.set_push_constants(RHI::ShaderStage::Fragment, 0, std::as_bytes(span<const HiZBuildConstants>{&constants, 1}));
                    pass.draw(RHI::DrawArgs{.vertex_count = 3});
                    return {};
                });

            source_extent = destination_extent;
        }

        pyramid.has_valid_data = true;
        return {};
    }

} // namespace SFT::Renderer
