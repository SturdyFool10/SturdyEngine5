#include <Foundation/src/Foundation.hpp>


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


using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {
    namespace {
        namespace slang = Core::Slang;

        struct BloomConstants {
            glm::vec2 source_texel_size{1.0f};
            f32 threshold = 1.0f;
            f32 soft_knee = 0.5f;
            f32 scatter = 0.7f;
        };
        static_assert(sizeof(BloomConstants) == 20);

        struct BloomCompositeConstants {
            f32 bloom_intensity = 0.0f;
        };
        static_assert(sizeof(BloomCompositeConstants) == 4);

        [[nodiscard]] Core::GraphicsBackendError bloom_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }
    } // namespace

    Core::RendererResult Renderer::ensure_bloom_resources(RHI::Format color_format) {
        auto guard = bloom_.lock();
        if (guard->ready && guard->color_format == color_format) return {};
        if (guard->ready) destroy_bloom_resources_locked(*guard);

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) return unexpected(bloom_error("Cannot build bloom resources without an RHI device."));

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "prefilterMain", .stage = slang::ShaderStage::Fragment},
                slang::ShaderEntryPointRequest{.name = "downsampleMain", .stage = slang::ShaderStage::Fragment},
                slang::ShaderEntryPointRequest{.name = "upsampleMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file("Shaders/fullscreen_bloom.slang", "fullscreen_bloom"), options);
        if (!shader) return unexpected(bloom_error("compile bloom shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->prefilter_entry_point = "prefilterMain";
        guard->downsample_entry_point = "downsampleMain";
        guard->upsample_entry_point = "upsampleMain";

        auto create_module = [&](string_view entry, const char *label) -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = guard->shader.entry_point_code(entry);
            if (!code) return unexpected(bloom_error("generate bloom shader bytecode failed: " + code.error().message));
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
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
                target.color = RHI::BlendComponent{.src_factor = RHI::BlendFactor::One, .dst_factor = RHI::BlendFactor::One, .op = RHI::BlendOp::Add};
                target.alpha = RHI::BlendComponent{.src_factor = RHI::BlendFactor::Zero, .dst_factor = RHI::BlendFactor::One, .op = RHI::BlendOp::Add};
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
        guard->ready = true;
        return {};
    }

    Core::RendererResult Renderer::record_bloom_draw(RHI::RenderPassEncoder &pass,
                                                       RHI::TextureViewHandle source_view, glm::vec2 source_texel_size,
                                                       f32 threshold, f32 soft_knee, f32 scatter,
                                                       bool prefilter, bool upsample,
                                                       RHI::BindGroupHandle bind_group) {
        auto guard = bloom_.lock();
        BloomResources &resources = *guard;
        if (!source_view || !bind_group || !resources.ready) return unexpected(bloom_error("Cannot record bloom without ready resources, a source texture, and a cached bind group."));

        const RHI::RenderPipelineHandle pipeline = upsample
            ? resources.upsample_pipeline
            : (prefilter ? resources.prefilter_pipeline : resources.downsample_pipeline);
        const BloomConstants constants{
            .source_texel_size = source_texel_size,
            .threshold = threshold,
            .soft_knee = soft_knee,
            .scatter = scatter,
        };
        pass.set_pipeline(pipeline);
        pass.set_bind_group(resources.sampled_set, bind_group);
        pass.set_push_constants(RHI::ShaderStage::Fragment, 0, std::as_bytes(span<const BloomConstants>{&constants, 1}));
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    Core::RendererResult Renderer::record_bloom_downsample(RHI::RenderPassEncoder &pass, RHI::TextureViewHandle source_view,
                                                            glm::vec2 source_texel_size, const RenderGraphSettings &settings,
                                                            bool apply_threshold, RHI::BindGroupHandle bind_group) {
        return record_bloom_draw(pass, source_view, source_texel_size,
                                 settings.bloom_threshold, settings.bloom_soft_knee, settings.bloom_scatter,
                                 apply_threshold, false, bind_group);
    }

    Core::RendererResult Renderer::record_bloom_upsample(RHI::RenderPassEncoder &pass, RHI::TextureViewHandle source_view,
                                                          glm::vec2 source_texel_size, const RenderGraphSettings &settings,
                                                          RHI::BindGroupHandle bind_group) {
        return record_bloom_draw(pass, source_view, source_texel_size,
                                 settings.bloom_threshold, settings.bloom_soft_knee, settings.bloom_scatter,
                                 false, true, bind_group);
    }

    void Renderer::destroy_bloom_resources() noexcept { auto guard = bloom_.lock(); destroy_bloom_resources_locked(*guard); }

    void Renderer::destroy_bloom_resources_locked(BloomResources &resources) noexcept {
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

    Core::RendererExpected<RHI::BindGroupHandle> Renderer::create_bloom_source_bind_group(RHI::TextureViewHandle source_view) {
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
            .label = "transient bloom source bind group",
        });
        if (!group) return unexpected(graphics_error_from_rhi(group.error(), "create transient bloom source bind group"));
        return *group;
    }

    Core::RendererResult Renderer::ensure_bloom_composite_resources() {
        auto guard = bloom_composite_.lock();
        if (guard->ready) return {};

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) return unexpected(bloom_error("Cannot build bloom composite resources without an RHI device."));

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file("Shaders/fullscreen_bloom_composite.slang", "fullscreen_bloom_composite"), options);
        if (!shader) return unexpected(bloom_error("compile bloom composite shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->fragment_entry_point = "fragmentMain";

        auto vertex_code = guard->shader.entry_point_code(guard->vertex_entry_point);
        if (!vertex_code) return unexpected(bloom_error("generate bloom composite vertex bytecode failed: " + vertex_code.error().message));
        auto vertex_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "bloom composite vertex module",
        });
        if (!vertex_module) return unexpected(graphics_error_from_rhi(vertex_module.error(), "create bloom composite vertex module"));
        guard->vertex_module = *vertex_module;

        auto fragment_code = guard->shader.entry_point_code(guard->fragment_entry_point);
        if (!fragment_code) {
            destroy_bloom_composite_resources_locked(*guard);
            return unexpected(bloom_error("generate bloom composite fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
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

        // Track the first two SampledTexture bindings in reflection order: fullscreen_bloom_composite.slang
        // declares sceneTexture before bloomTexture, so entry order matches declaration order the same way
        // the (now-reverted) two-texture tonemap prototype relied on.
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

        guard->ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::bloom_composite_pipeline_for(RHI::Format color_format) {
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

    Core::RendererResult Renderer::record_bloom_composite(RHI::RenderPassEncoder &pass,
                                                           RHI::TextureViewHandle scene_view,
                                                           RHI::TextureViewHandle bloom_view,
                                                           RHI::Format color_format,
                                                           f32 bloom_intensity,
                                                           vector<RHI::BindGroupHandle> &transient_bind_groups) {
        auto pipeline = bloom_composite_pipeline_for(color_format);
        if (!pipeline) return unexpected(pipeline.error());
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !scene_view || !bloom_view) {
            return unexpected(bloom_error("Cannot record the bloom composite pass without a device, scene texture, and bloom texture."));
        }

        auto guard = bloom_composite_.lock();
        const array<RHI::BindGroupEntry, 3> entries{
            RHI::BindGroupEntry{.binding = guard->scene_binding, .texture_view = scene_view},
            RHI::BindGroupEntry{.binding = guard->bloom_binding, .texture_view = bloom_view},
            RHI::BindGroupEntry{.binding = guard->sampler_binding, .sampler = guard->sampler},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = guard->bind_group_layouts.front(),
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "bloom composite bind group",
        });
        if (!bind_group) return unexpected(graphics_error_from_rhi(bind_group.error(), "create bloom composite bind group"));
        transient_bind_groups.push_back(*bind_group);

        pass.set_pipeline(*pipeline);
        pass.set_bind_group(guard->bind_group_layout_sets.front(), *bind_group);
        const BloomCompositeConstants constants{.bloom_intensity = bloom_intensity};
        pass.set_push_constants(RHI::ShaderStage::Fragment, 0, std::as_bytes(span<const BloomCompositeConstants>{&constants, 1}));
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_bloom_composite_resources() noexcept {
        auto guard = bloom_composite_.lock();
        destroy_bloom_composite_resources_locked(*guard);
    }

    void Renderer::destroy_bloom_composite_resources_locked(BloomCompositeResources &resources) noexcept {
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
