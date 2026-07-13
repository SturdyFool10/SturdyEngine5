module;

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

module Sturdy.Renderer;

import :Renderer;
import :ReflectionBinding;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.RHI;

using std::array;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {

    namespace {
        namespace slang = Core::Slang;

        [[nodiscard]] Core::GraphicsBackendError tonemap_error(string message) {
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

    Core::RendererResult Renderer::ensure_tonemap_resources() {
        if (tonemap_.ready) {
            return {};
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(tonemap_error("Cannot build tonemap resources without an RHI device."));
        }

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = "fragmentMain", .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file("Shaders/fullscreen_tonemap.slang", "fullscreen_tonemap"), options);
        if (!shader) {
            return unexpected(tonemap_error("compile tonemap shader failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }
        tonemap_.shader = *shader;
        tonemap_.vertex_entry_point = "vertexMain";
        tonemap_.fragment_entry_point = "fragmentMain";

        auto vertex_code = tonemap_.shader.entry_point_code(tonemap_.vertex_entry_point);
        if (!vertex_code) {
            return unexpected(tonemap_error("generate tonemap vertex bytecode failed: " + vertex_code.error().message));
        }
        auto vertex_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{vertex_code->bytes.data(), vertex_code->bytes.size()},
            .label = "tonemap vertex module",
        });
        if (!vertex_module) {
            return unexpected(graphics_error_from_rhi(vertex_module.error(), "create tonemap vertex module"));
        }
        tonemap_.vertex_module = *vertex_module;

        auto fragment_code = tonemap_.shader.entry_point_code(tonemap_.fragment_entry_point);
        if (!fragment_code) {
            destroy_tonemap_resources();
            return unexpected(tonemap_error("generate tonemap fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "tonemap fragment module",
        });
        if (!fragment_module) {
            destroy_tonemap_resources();
            return unexpected(graphics_error_from_rhi(fragment_module.error(), "create tonemap fragment module"));
        }
        tonemap_.fragment_module = *fragment_module;

        // Bind-group + pipeline layouts, generated from the shader's (Texture2D + SamplerState) bindings.
        const slang::ShaderReflection &reflection = tonemap_.shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "tonemap bind group layout",
            });
            if (!handle) {
                destroy_tonemap_resources();
                return unexpected(graphics_error_from_rhi(handle.error(), "create tonemap bind group layout"));
            }
            tonemap_.bind_group_layouts.push_back(*handle);
            tonemap_.bind_group_layout_sets.push_back(layout.set);
        }
        if (tonemap_.bind_group_layouts.empty()) {
            destroy_tonemap_resources();
            return unexpected(tonemap_error("tonemap shader produced no bind-group layout (expected a texture + sampler)."));
        }

        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{tonemap_.bind_group_layouts.data(), tonemap_.bind_group_layouts.size()},
            .push_constant_ranges = {},
            .label = "tonemap pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_tonemap_resources();
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create tonemap pipeline layout"));
        }
        tonemap_.pipeline_layout = *pipeline_layout;

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "tonemap scene sampler",
        });
        if (!sampler) {
            destroy_tonemap_resources();
            return unexpected(graphics_error_from_rhi(sampler.error(), "create tonemap sampler"));
        }
        tonemap_.sampler = *sampler;

        tonemap_.ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::tonemap_pipeline_for(RHI::Format color_format) {
        if (Core::RendererResult ready = ensure_tonemap_resources(); !ready) {
            return unexpected(ready.error());
        }
        for (const TonemapPipelineVariant &variant : tonemap_.pipeline_variants) {
            if (variant.color_format == color_format) {
                return variant.pipeline;
            }
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(tonemap_error("Cannot build a tonemap pipeline without an RHI device."));
        }

        const RHI::ColorTargetState color_target{.format = color_format, .blend_enable = false, .write_mask = RHI::ColorWriteMask::All};
        // No vertex buffers (fullscreen triangle from SV_VertexID) and no depth attachment.
        const RHI::RenderPipelineDesc desc{
            .layout = tonemap_.pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = tonemap_.vertex_module, .entry_point = tonemap_.vertex_entry_point.c_str(), .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = tonemap_.fragment_module, .entry_point = tonemap_.fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {},
            .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&color_target, 1},
            .label = "tonemap pipeline",
        };
        auto pipeline = device->create_render_pipeline(desc);
        if (!pipeline) {
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create tonemap pipeline"));
        }
        tonemap_.pipeline_variants.push_back(TonemapPipelineVariant{.color_format = color_format, .pipeline = *pipeline});
        return *pipeline;
    }

    Core::RendererResult Renderer::record_tonemap(RHI::RenderPassEncoder &pass, RHI::TextureViewHandle source_view,
                                                  RHI::Format color_format) {
        auto pipeline = tonemap_pipeline_for(color_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !source_view) {
            return unexpected(tonemap_error("Cannot record the tonemap pass without a device and scene texture."));
        }

        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(tonemap_.shader.reflection(),
                                                                                       reflected_stage_mask(tonemap_.shader.reflection()));
        if (generated.empty()) {
            return unexpected(tonemap_error("tonemap shader reflection produced no bind-group layout."));
        }
        const GeneratedBindGroupLayout &layout = generated.front();
        u32 image_binding = 0;
        u32 sampler_binding = 0;
        bool has_image_binding = false;
        bool has_sampler_binding = false;
        for (const RHI::BindGroupLayoutEntry &entry : layout.entries) {
            if (entry.type == RHI::BindingType::SampledTexture && !has_image_binding) {
                image_binding = entry.binding;
                has_image_binding = true;
            } else if (entry.type == RHI::BindingType::Sampler && !has_sampler_binding) {
                sampler_binding = entry.binding;
                has_sampler_binding = true;
            }
        }
        if (!has_image_binding || !has_sampler_binding) {
            return unexpected(tonemap_error("tonemap shader reflection did not produce one sampled texture and one sampler."));
        }
        const usize layout_index = bind_group_layout_index_for_set(tonemap_.bind_group_layout_sets, layout.set);
        if (layout_index >= tonemap_.bind_group_layouts.size()) {
            return unexpected(tonemap_error("tonemap shader reflection set has no generated bind-group layout."));
        }
        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = image_binding, .texture_view = source_view},
            RHI::BindGroupEntry{.binding = sampler_binding, .sampler = tonemap_.sampler},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = tonemap_.bind_group_layouts[layout_index],
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "tonemap scene bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create tonemap bind group"));
        }
        // Freed after the frame fence retires (destroy_all_resources / the per-frame cleanup path).
        frame_transient_bind_groups_.push_back(*bind_group);

        pass.set_pipeline(*pipeline);
        pass.set_bind_group(layout.set, *bind_group);
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_tonemap_resources() noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            tonemap_ = {};
            return;
        }
        for (const TonemapPipelineVariant &variant : tonemap_.pipeline_variants) {
            if (variant.pipeline) {
                device->destroy_render_pipeline(variant.pipeline);
            }
        }
        if (tonemap_.sampler) {
            device->destroy_sampler(tonemap_.sampler);
        }
        if (tonemap_.pipeline_layout) {
            device->destroy_pipeline_layout(tonemap_.pipeline_layout);
        }
        for (RHI::BindGroupLayoutHandle layout : tonemap_.bind_group_layouts) {
            device->destroy_bind_group_layout(layout);
        }
        if (tonemap_.fragment_module) {
            device->destroy_shader_module(tonemap_.fragment_module);
        }
        if (tonemap_.vertex_module) {
            device->destroy_shader_module(tonemap_.vertex_module);
        }
        tonemap_ = {};
    }

} // namespace SFT::Renderer
