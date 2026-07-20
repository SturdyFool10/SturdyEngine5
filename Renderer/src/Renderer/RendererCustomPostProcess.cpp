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

        [[nodiscard]] Core::GraphicsBackendError custom_effect_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }
    } // namespace

    Core::RendererResult Renderer::ensure_custom_post_process(const CustomPostProcessEffect &effect,
                                                               RHI::Format color_format) {
        if (effect.shader_path.empty() || effect.module_name.empty() || effect.fragment_entry_point.empty()) {
            return unexpected(custom_effect_error("Custom post-process requires shader_path, module_name, and fragment_entry_point."));
        }
        auto resources = custom_post_process_resources_.lock();
        for (const CustomPostProcessResources &resource : *resources) {
            if (resource.shader_path == effect.shader_path && resource.module_name == effect.module_name &&
                resource.fragment_entry_point == effect.fragment_entry_point && resource.color_format == color_format) {
                return {};
            }
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) return unexpected(custom_effect_error("Cannot build custom post-process without an RHI device."));

        CustomPostProcessResources resource{
            .shader_path = effect.shader_path,
            .module_name = effect.module_name,
            .fragment_entry_point = effect.fragment_entry_point,
            .color_format = color_format,
        };
        auto cleanup = [&]() noexcept {
            if (resource.pipeline) device->destroy_render_pipeline(resource.pipeline);
            if (resource.sampler) device->destroy_sampler(resource.sampler);
            if (resource.pipeline_layout) device->destroy_pipeline_layout(resource.pipeline_layout);
            if (resource.bind_group_layout) device->destroy_bind_group_layout(resource.bind_group_layout);
            if (resource.fragment_module) device->destroy_shader_module(resource.fragment_module);
            if (resource.vertex_module) device->destroy_shader_module(resource.vertex_module);
        };

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = "vertexMain", .stage = slang::ShaderStage::Vertex},
                slang::ShaderEntryPointRequest{.name = effect.fragment_entry_point, .stage = slang::ShaderStage::Fragment},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file(effect.shader_path, effect.module_name), options);
        if (!shader) return unexpected(custom_effect_error("compile custom post-process failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        resource.shader = *shader;

        auto create_module = [&](string_view entry, const char *label) -> Core::RendererExpected<RHI::ShaderModuleHandle> {
            auto code = resource.shader.entry_point_code(entry);
            if (!code) return unexpected(custom_effect_error("generate custom post-process bytecode failed: " + code.error().message));
            auto module = device->create_shader_module(RHI::ShaderModuleDesc{
                .language = RHI::ShaderLanguage::SpirV,
                .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
                .label = label,
            });
            if (!module) return unexpected(graphics_error_from_rhi(module.error(), label));
            return *module;
        };
        auto vertex = create_module("vertexMain", "custom post-process vertex module");
        if (!vertex) return unexpected(vertex.error());
        resource.vertex_module = *vertex;
        auto fragment = create_module(effect.fragment_entry_point, "custom post-process fragment module");
        if (!fragment) { cleanup(); return unexpected(fragment.error()); }
        resource.fragment_module = *fragment;

        const slang::ShaderReflection &reflection = resource.shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, reflected_stage_mask(reflection));
        if (generated.size() != 1) {
            cleanup();
            return unexpected(custom_effect_error("Custom post-process shader must expose exactly one texture/sampler bind group."));
        }
        const GeneratedBindGroupLayout &layout = generated.front();
        bool has_image = false;
        bool has_sampler = false;
        for (const RHI::BindGroupLayoutEntry &entry : layout.entries) {
            if (entry.type == RHI::BindingType::SampledTexture) {
                resource.image_binding = entry.binding;
                has_image = true;
            } else if (entry.type == RHI::BindingType::Sampler) {
                resource.sampler_binding = entry.binding;
                has_sampler = true;
            }
        }
        if (!has_image || !has_sampler || layout.entries.size() != 2) {
            cleanup();
            return unexpected(custom_effect_error("Custom post-process shader must expose exactly one sampled texture and one sampler."));
        }
        resource.set = layout.set;
        auto bind_layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
            .label = "custom post-process bind group layout",
        });
        if (!bind_layout) { cleanup(); return unexpected(graphics_error_from_rhi(bind_layout.error(), "create custom post-process bind group layout")); }
        resource.bind_group_layout = *bind_layout;

        const vector<RHI::PushConstantRange> push_ranges = generate_push_constant_ranges(reflection, RHI::ShaderStage::Fragment);
        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{&resource.bind_group_layout, 1},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "custom post-process pipeline layout",
        });
        if (!pipeline_layout) { cleanup(); return unexpected(graphics_error_from_rhi(pipeline_layout.error(), "create custom post-process pipeline layout")); }
        resource.pipeline_layout = *pipeline_layout;

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear, .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge, .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge, .max_lod = 0.0f,
            .label = "custom post-process sampler",
        });
        if (!sampler) { cleanup(); return unexpected(graphics_error_from_rhi(sampler.error(), "create custom post-process sampler")); }
        resource.sampler = *sampler;

        const RHI::ColorTargetState target{.format = color_format, .blend_enable = false, .write_mask = RHI::ColorWriteMask::All};
        auto pipeline = device->create_render_pipeline(RHI::RenderPipelineDesc{
            .layout = resource.pipeline_layout,
            .vertex = RHI::ShaderEntry{.module = resource.vertex_module, .entry_point = "vertexMain", .stage = RHI::ShaderStage::Vertex},
            .fragment = RHI::ShaderEntry{.module = resource.fragment_module, .entry_point = resource.fragment_entry_point.c_str(), .stage = RHI::ShaderStage::Fragment},
            .vertex_buffers = {}, .topology = RHI::PrimitiveTopology::TriangleList,
            .rasterization = RHI::RasterizationState{.cull_mode = RHI::CullMode::None},
            .depth_stencil = RHI::DepthStencilState{},
            .color_targets = span<const RHI::ColorTargetState>{&target, 1},
            .label = "custom post-process pipeline",
        });
        if (!pipeline) { cleanup(); return unexpected(graphics_error_from_rhi(pipeline.error(), "create custom post-process pipeline")); }
        resource.pipeline = *pipeline;
        resources->push_back(std::move(resource));
        return {};
    }

    Core::RendererResult Renderer::record_custom_post_process(RHI::RenderPassEncoder &pass,
                                                               RHI::TextureViewHandle source_view,
                                                               RHI::Format color_format,
                                                               const CustomPostProcessEffect &effect,
                                                               vector<RHI::BindGroupHandle> &transient_bind_groups) {
        if (Core::RendererResult ready = ensure_custom_post_process(effect, color_format); !ready) return ready;
        auto resources = custom_post_process_resources_.lock();
        CustomPostProcessResources *resource = nullptr;
        for (CustomPostProcessResources &candidate : *resources) {
            if (candidate.shader_path == effect.shader_path && candidate.module_name == effect.module_name &&
                candidate.fragment_entry_point == effect.fragment_entry_point && candidate.color_format == color_format) {
                resource = &candidate;
                break;
            }
        }
        if (resource == nullptr) return unexpected(custom_effect_error("Custom post-process cache lookup failed."));
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr || !source_view) return unexpected(custom_effect_error("Cannot record custom post-process without device/source view."));

        const array<RHI::BindGroupEntry, 2> entries{
            RHI::BindGroupEntry{.binding = resource->image_binding, .texture_view = source_view},
            RHI::BindGroupEntry{.binding = resource->sampler_binding, .sampler = resource->sampler},
        };
        auto group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = resource->bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "custom post-process bind group",
        });
        if (!group) return unexpected(graphics_error_from_rhi(group.error(), "create custom post-process bind group"));
        transient_bind_groups.push_back(*group);

        pass.set_pipeline(resource->pipeline);
        pass.set_bind_group(resource->set, *group);
        if (!effect.push_constants.empty()) {
            pass.set_push_constants(RHI::ShaderStage::Fragment, 0,
                                    span<const std::byte>{effect.push_constants.data(), effect.push_constants.size()});
        }
        pass.draw(RHI::DrawArgs{.vertex_count = 3});
        return {};
    }

    void Renderer::destroy_custom_post_process_resources() noexcept {
        auto resources = custom_post_process_resources_.lock();
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (CustomPostProcessResources &resource : *resources) {
                if (resource.pipeline) device->destroy_render_pipeline(resource.pipeline);
                if (resource.sampler) device->destroy_sampler(resource.sampler);
                if (resource.pipeline_layout) device->destroy_pipeline_layout(resource.pipeline_layout);
                if (resource.bind_group_layout) device->destroy_bind_group_layout(resource.bind_group_layout);
                if (resource.fragment_module) device->destroy_shader_module(resource.fragment_module);
                if (resource.vertex_module) device->destroy_shader_module(resource.vertex_module);
            }
        }
        resources->clear();
    }
} // namespace SFT::Renderer
