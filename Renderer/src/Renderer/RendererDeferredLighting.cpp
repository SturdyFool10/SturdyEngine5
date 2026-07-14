#include <Foundation/Foundation.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Renderer/RendererModule.hpp>
#include <Renderer/ReflectionBinding.hpp>
#include <Core/Core.hpp>
#include <RHI/RHI.hpp>

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError deferred_lighting_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }

        [[nodiscard]] usize bind_group_layout_index_for_set(span<const u32> sets, u32 set) noexcept {
            for (usize i = 0; i < sets.size(); ++i) {
                if (sets[i] == set) {
                    return i;
                }
            }
            return sets.size();
        }
    } // namespace

    Core::RendererResult Renderer::ensure_deferred_lighting_resources() {
        auto guard = deferred_lighting_.lock();
        if (guard->ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(deferred_lighting_error("Cannot build deferred lighting resources without an RHI device."));
        }

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file("Shaders/deferred_lighting.slang", "deferred_lighting"), options);
        if (!shader) {
            return unexpected(deferred_lighting_error("compile deferred lighting shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->fragment_entry_point = "fragmentMain";

        auto vertex_code = guard->shader.entry_point_code(guard->vertex_entry_point);
        if (!vertex_code) {
            return unexpected(deferred_lighting_error("generate deferred lighting vertex bytecode failed: " + vertex_code.error().message));
        }
        auto vertex_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "deferred lighting vertex module",
        });
        if (!vertex_module) {
            return unexpected(graphics_error_from_rhi(vertex_module.error(), "create deferred lighting vertex module"));
        }
        guard->vertex_module = *vertex_module;

        auto fragment_code = guard->shader.entry_point_code(guard->fragment_entry_point);
        if (!fragment_code) {
            destroy_deferred_lighting_resources_locked(*guard);
            return unexpected(deferred_lighting_error("generate deferred lighting fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "deferred lighting fragment module",
        });
        if (!fragment_module) {
            destroy_deferred_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(fragment_module.error(), "create deferred lighting fragment module"));
        }
        guard->fragment_module = *fragment_module;

        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "deferred lighting bind group layout",
            });
            if (!handle) {
                destroy_deferred_lighting_resources_locked(*guard);
                return unexpected(graphics_error_from_rhi(handle.error(), "create deferred lighting bind group layout"));
            }
            guard->bind_group_layouts.push_back(*handle);
            guard->bind_group_layout_sets.push_back(layout.set);
        }
        if (guard->bind_group_layouts.empty()) {
            destroy_deferred_lighting_resources_locked(*guard);
            return unexpected(deferred_lighting_error("deferred lighting shader produced no bind-group layout."));
        }

        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{guard->bind_group_layouts.data(), guard->bind_group_layouts.size()},
            .push_constant_ranges = {},
            .label = "deferred lighting pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_deferred_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create deferred lighting pipeline layout"));
        }
        guard->pipeline_layout = *pipeline_layout;

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Nearest,
            .mag_filter = RHI::Filter::Nearest,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "deferred lighting gbuffer sampler",
        });
        if (!sampler) {
            destroy_deferred_lighting_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(sampler.error(), "create deferred lighting sampler"));
        }
        guard->sampler = *sampler;

        guard->ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::deferred_lighting_pipeline_for(RHI::Format color_format) {
        if (Core::RendererResult ready = ensure_deferred_lighting_resources(); !ready) {
            return unexpected(ready.error());
        }

        auto guard = deferred_lighting_.lock();
        for (const DeferredLightingPipelineVariant &variant : guard->pipeline_variants) {
            if (variant.color_format == color_format) {
                return variant.pipeline;
            }
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(deferred_lighting_error("Cannot build a deferred lighting pipeline without an RHI device."));
        }

        const RHI::ColorTargetState color_target{.format = color_format, .blend_enable = false, .write_mask = RHI::ColorWriteMask::All};
        const RHI::RenderPipelineDesc desc{
            .layout = guard->pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module, .entry_point = guard->vertex_entry_point.c_str(), .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = guard->fragment_module, .entry_point = guard->fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "deferred lighting pipeline",
        };
        auto pipeline = device->create_render_pipeline(desc);
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create deferred lighting pipeline"));
        }
        guard->pipeline_variants.push_back(DeferredLightingPipelineVariant{.color_format = color_format, .pipeline = *pipeline});
        return *pipeline;
    }

    Core::RendererResult Renderer::record_deferred_lighting(RHI::RenderPassEncoder &pass,
                                                            RHI::TextureViewHandle albedo_view,
                                                            RHI::TextureViewHandle normal_view,
                                                            RHI::TextureViewHandle material_view,
                                                            RHI::Format color_format,
                                                            vector<RHI::BindGroupHandle> &transient_bind_groups) {
        auto pipeline = deferred_lighting_pipeline_for(color_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !albedo_view || !normal_view || !material_view) {
            return unexpected(deferred_lighting_error("Cannot record deferred lighting without a device and valid G-buffer views."));
        }

        auto guard = deferred_lighting_.lock();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(guard->shader.reflection(),
                                                                                       reflected_stage_mask(guard->shader.reflection()));
        if (generated.empty()) {
            return unexpected(deferred_lighting_error("deferred lighting shader reflection produced no bind-group layout."));
        }
        const GeneratedBindGroupLayout &layout = generated.front();
        vector<u32> image_bindings;
        u32 sampler_binding = 0;
        bool has_sampler_binding = false;
        for (const RHI::BindGroupLayoutEntry &entry : layout.entries) {
            if (entry.type == RHI::BindingType::SampledTexture) {
                image_bindings.push_back(entry.binding);
            } else if (entry.type == RHI::BindingType::Sampler) {
                sampler_binding = entry.binding;
                has_sampler_binding = true;
            }
        }
        if (image_bindings.size() < 3 || !has_sampler_binding) {
            return unexpected(deferred_lighting_error("deferred lighting shader reflection did not produce three sampled textures and one sampler."));
        }
        const usize layout_index = bind_group_layout_index_for_set(guard->bind_group_layout_sets, layout.set);
        if (layout_index >= guard->bind_group_layouts.size()) {
            return unexpected(deferred_lighting_error("deferred lighting shader reflection set has no generated bind-group layout."));
        }
        const array<RHI::BindGroupEntry, 4> entries{
            RHI::BindGroupEntry{.binding = image_bindings[0], .texture_view = albedo_view},
            RHI::BindGroupEntry{.binding = image_bindings[1], .texture_view = normal_view},
            RHI::BindGroupEntry{.binding = image_bindings[2], .texture_view = material_view},
            RHI::BindGroupEntry{.binding = sampler_binding, .sampler = guard->sampler},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = guard->bind_group_layouts[layout_index],
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "deferred lighting gbuffer bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create deferred lighting bind group"));
        }
        transient_bind_groups.push_back(*bind_group);

        pass.set_pipeline(*pipeline);
        pass.set_bind_group(layout.set, *bind_group);
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_deferred_lighting_resources() noexcept {
        auto guard = deferred_lighting_.lock();
        destroy_deferred_lighting_resources_locked(*guard);
    }

    void Renderer::destroy_deferred_lighting_resources_locked(DeferredLightingResources &resources) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            resources = {};
            return;
        }
        for (const DeferredLightingPipelineVariant &variant : resources.pipeline_variants) {
            if (variant.pipeline) {
                device->destroy_render_pipeline(variant.pipeline);
            }
        }
        if (resources.sampler) {
            device->destroy_sampler(resources.sampler);
        }
        if (resources.pipeline_layout) {
            device->destroy_pipeline_layout(resources.pipeline_layout);
        }
        for (RHI::BindGroupLayoutHandle layout : resources.bind_group_layouts) {
            device->destroy_bind_group_layout(layout);
        }
        if (resources.fragment_module) {
            device->destroy_shader_module(resources.fragment_module);
        }
        if (resources.vertex_module) {
            device->destroy_shader_module(resources.vertex_module);
        }
        resources = {};
    }

} // namespace SFT::Renderer
