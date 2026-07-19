#include <Foundation/src/Foundation.hpp>

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

        // Deliberately no alignas(16): every field is a plain 4-byte scalar (no vec3/array members —
        // see Engine::PsychoVSettings's doc comment on why the gray-point vec3s are flattened into
        // separate r/g/b scalars here), so this struct's natural size already matches what Slang's
        // push_constant block reflects byte-for-byte. Forcing 16-byte alignment would pad the C++
        // side to a size the shader's reflected push-constant range doesn't agree with.
        struct TonemapConstants {
            f32 exposure = 1.0f;
            f32 white_point = 1.0f;
            f32 saturation = 1.0f;
            u32 operation = 0;

            u32 hdr_output = 0;
            f32 hdr_paper_white_nits = 203.0f;
            f32 hdr_peak_nits = 1000.0f;

            u32 agx_look = 0;

            f32 hermite_toe_strength = 0.5f;
            f32 hermite_toe_length = 0.5f;
            f32 hermite_shoulder_strength = 2.0f;
            f32 hermite_shoulder_length = 0.5f;
            f32 hermite_shoulder_angle = 1.0f;

            f32 psychov_highlights = 1.0f;
            f32 psychov_shadows = 1.0f;
            f32 psychov_contrast = 1.0f;
            f32 psychov_purity_scale = 1.0f;
            f32 psychov_gamut_compression = 1.0f;
            u32 psychov_gamut_compression_mode = 1;
            f32 psychov_compression = 0.0f;
            f32 psychov_adapted_gray_r = 0.18f;
            f32 psychov_adapted_gray_g = 0.18f;
            f32 psychov_adapted_gray_b = 0.18f;
            f32 psychov_background_gray_r = 0.18f;
            f32 psychov_background_gray_g = 0.18f;
            f32 psychov_background_gray_b = 0.18f;
        };
        static_assert(sizeof(TonemapConstants) == 104);

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
        auto guard = tonemap_.lock();
        if (guard->ready) {
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
        guard->shader = *shader;
        guard->vertex_entry_point = "vertexMain";
        guard->fragment_entry_point = "fragmentMain";

        auto vertex_code = guard->shader.entry_point_code(guard->vertex_entry_point);
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
        guard->vertex_module = *vertex_module;

        auto fragment_code = guard->shader.entry_point_code(guard->fragment_entry_point);
        if (!fragment_code) {
            destroy_tonemap_resources_locked(*guard);
            return unexpected(tonemap_error("generate tonemap fragment bytecode failed: " + fragment_code.error().message));
        }
        auto fragment_module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{fragment_code->bytes.data(), fragment_code->bytes.size()},
            .label = "tonemap fragment module",
        });
        if (!fragment_module) {
            destroy_tonemap_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(fragment_module.error(), "create tonemap fragment module"));
        }
        guard->fragment_module = *fragment_module;

        // Bind-group + pipeline layouts, generated from the shader's (Texture2D + SamplerState) bindings.
        const slang::ShaderReflection &reflection = guard->shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        for (const GeneratedBindGroupLayout &layout : generated) {
            auto handle = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
                .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
                .label = "tonemap bind group layout",
            });
            if (!handle) {
                destroy_tonemap_resources_locked(*guard);
                return unexpected(graphics_error_from_rhi(handle.error(), "create tonemap bind group layout"));
            }
            guard->bind_group_layouts.push_back(*handle);
            guard->bind_group_layout_sets.push_back(layout.set);
        }
        if (guard->bind_group_layouts.empty()) {
            destroy_tonemap_resources_locked(*guard);
            return unexpected(tonemap_error("tonemap shader produced no bind-group layout (expected a texture + sampler)."));
        }

        const vector<RHI::PushConstantRange> push_constant_ranges =
            generate_push_constant_ranges(reflection, RHI::ShaderStage::Fragment);
        if (push_constant_ranges.empty()) {
            destroy_tonemap_resources_locked(*guard);
            return unexpected(tonemap_error("tonemap shader produced no push-constant range."));
        }
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{guard->bind_group_layouts.data(), guard->bind_group_layouts.size()},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_constant_ranges.data(), push_constant_ranges.size()},
            .label = "tonemap pipeline layout",
        });
        if (!pipeline_layout) {
            destroy_tonemap_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create tonemap pipeline layout"));
        }
        guard->pipeline_layout = *pipeline_layout;

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
            destroy_tonemap_resources_locked(*guard);
            return unexpected(graphics_error_from_rhi(sampler.error(), "create tonemap sampler"));
        }
        guard->sampler = *sampler;

        guard->ready = true;
        return {};
    }

    Core::RendererExpected<RHI::RenderPipelineHandle> Renderer::tonemap_pipeline_for(RHI::Format color_format) {
        if (Core::RendererResult ready = ensure_tonemap_resources(); !ready) {
            return unexpected(ready.error());
        }

        auto guard = tonemap_.lock();
        for (const TonemapPipelineVariant &variant : guard->pipeline_variants) {
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
            .layout = guard->pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = guard->vertex_module, .entry_point = guard->vertex_entry_point.c_str(), .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = guard->fragment_module, .entry_point = guard->fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment},
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
        guard->pipeline_variants.push_back(TonemapPipelineVariant{.color_format = color_format, .pipeline = *pipeline});
        return *pipeline;
    }

    Core::RendererResult Renderer::record_tonemap(RHI::RenderPassEncoder &pass, RHI::TextureViewHandle source_view,
                                                  RHI::Format color_format, const RenderGraphSettings &settings,
                                                  vector<RHI::BindGroupHandle> &transient_bind_groups) {
        auto pipeline = tonemap_pipeline_for(color_format);
        if (!pipeline) {
            return unexpected(pipeline.error());
        }
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !source_view) {
            return unexpected(tonemap_error("Cannot record the tonemap pass without a device and scene texture."));
        }

        auto guard = tonemap_.lock();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(guard->shader.reflection(),
                                                                                       reflected_stage_mask(guard->shader.reflection()));
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
        const usize layout_index = bind_group_layout_index_for_set(guard->bind_group_layout_sets, layout.set);
        if (layout_index >= guard->bind_group_layouts.size()) {
            return unexpected(tonemap_error("tonemap shader reflection set has no generated bind-group layout."));
        }
        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = image_binding, .texture_view = source_view},
            RHI::BindGroupEntry{.binding = sampler_binding, .sampler = guard->sampler},
        };
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = guard->bind_group_layouts[layout_index],
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "tonemap scene bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), "create tonemap bind group"));
        }
        // Freed after the frame fence retires (this frame's FrameInFlight slot).
        transient_bind_groups.push_back(*bind_group);

        pass.set_pipeline(*pipeline);
        pass.set_bind_group(layout.set, *bind_group);
        const ToneMappingOperator operation = settings.tone_mapping
            ? settings.tone_mapping_operator
            : ToneMappingOperator::None;
        const TonemapConstants constants{
            .exposure = settings.tone_mapping_exposure,
            .white_point = settings.tone_mapping_white_point,
            .saturation = settings.tone_mapping_saturation,
            .operation = static_cast<u32>(operation),
            .hdr_output = static_cast<u32>(settings.tone_mapping_hdr_output),
            .hdr_paper_white_nits = settings.tone_mapping_hdr_paper_white_nits,
            .hdr_peak_nits = settings.tone_mapping_hdr_peak_nits,
            .agx_look = static_cast<u32>(settings.agx_look),
            .hermite_toe_strength = settings.hermite_toe_strength,
            .hermite_toe_length = settings.hermite_toe_length,
            .hermite_shoulder_strength = settings.hermite_shoulder_strength,
            .hermite_shoulder_length = settings.hermite_shoulder_length,
            .hermite_shoulder_angle = settings.hermite_shoulder_angle,
            .psychov_highlights = settings.psychov_highlights,
            .psychov_shadows = settings.psychov_shadows,
            .psychov_contrast = settings.psychov_contrast,
            .psychov_purity_scale = settings.psychov_purity_scale,
            .psychov_gamut_compression = settings.psychov_gamut_compression,
            .psychov_gamut_compression_mode = settings.psychov_gamut_compression_use_bt2020 ? 1u : 0u,
            .psychov_compression = settings.psychov_compression,
            .psychov_adapted_gray_r = settings.psychov_adapted_gray_bt709.r,
            .psychov_adapted_gray_g = settings.psychov_adapted_gray_bt709.g,
            .psychov_adapted_gray_b = settings.psychov_adapted_gray_bt709.b,
            .psychov_background_gray_r = settings.psychov_background_gray_bt709.r,
            .psychov_background_gray_g = settings.psychov_background_gray_bt709.g,
            .psychov_background_gray_b = settings.psychov_background_gray_bt709.b,
        };
        pass.set_push_constants(RHI::ShaderStage::Fragment, 0,
                                std::as_bytes(span<const TonemapConstants>{&constants, 1}));
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_tonemap_resources() noexcept {
        auto guard = tonemap_.lock();
        destroy_tonemap_resources_locked(*guard);
    }

    void Renderer::destroy_tonemap_resources_locked(TonemapResources &resources) noexcept {
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            resources = {};
            return;
        }
        for (const TonemapPipelineVariant &variant : resources.pipeline_variants) {
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
