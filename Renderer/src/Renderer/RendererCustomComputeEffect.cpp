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
        [[nodiscard]] Core::GraphicsBackendError custom_compute_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }
    } // namespace

    Core::RendererResult Renderer::ensure_custom_compute_effect(const CustomComputeEffect &effect) {
        if (effect.shader_path.empty() || effect.module_name.empty() || effect.compute_entry_point.empty()) {
            return unexpected(custom_compute_error(
                "Custom compute effect requires shader_path, module_name, and compute_entry_point."));
        }

        auto resources = custom_compute_effect_resources_.lock();
        for (const CustomComputeEffectResources &resource : *resources) {
            if (resource.shader_path == effect.shader_path && resource.module_name == effect.module_name &&
                resource.compute_entry_point == effect.compute_entry_point) {
                if (effect.push_constants.size() != resource.push_constant_size) {
                    return unexpected(custom_compute_error(
                        "Custom compute push-constant payload size does not match the shader's reflected range."));
                }
                return {};
            }
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(custom_compute_error("Cannot build a custom compute effect without an RHI device."));
        }

        CustomComputeEffectResources resource{
            .shader_path = effect.shader_path,
            .module_name = effect.module_name,
            .compute_entry_point = effect.compute_entry_point,
        };
        auto cleanup = [&]() noexcept {
            if (resource.pipeline) device->destroy_compute_pipeline(resource.pipeline);
            if (resource.sampler) device->destroy_sampler(resource.sampler);
            if (resource.pipeline_layout) device->destroy_pipeline_layout(resource.pipeline_layout);
            if (resource.bind_group_layout) device->destroy_bind_group_layout(resource.bind_group_layout);
            if (resource.module) device->destroy_shader_module(resource.module);
        };

        const slang::ShaderCompileOptions options{
            .targets = {slang::ShaderTarget{}},
            .entry_points = {
                slang::ShaderEntryPointRequest{.name = effect.compute_entry_point, .stage = slang::ShaderStage::Compute},
            },
        };
        slang::ShaderCompiler compiler;
        auto shader = compiler.compile(slang::ShaderSource::from_file(effect.shader_path, effect.module_name), options);
        if (!shader) {
            return unexpected(custom_compute_error(
                "compile custom compute effect failed: " + shader.error().message + "\n" + shader.error().diagnostics));
        }
        resource.shader = *shader;

        auto code = resource.shader.entry_point_code(effect.compute_entry_point);
        if (!code) {
            return unexpected(custom_compute_error(
                "generate custom compute effect bytecode failed: " + code.error().message));
        }
        auto module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = RHI::ShaderLanguage::SpirV,
            .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
            .label = "custom compute effect module",
        });
        if (!module) {
            return unexpected(graphics_error_from_rhi(module.error(), "create custom compute effect shader module"));
        }
        resource.module = *module;

        const slang::ShaderReflection &reflection = resource.shader.reflection();
        bool has_supported_entry_point = false;
        for (const slang::ShaderEntryPointReflection &entry : reflection.entry_points) {
            const bool name_matches = entry.name == effect.compute_entry_point ||
                                      entry.name_override == effect.compute_entry_point;
            if (name_matches && entry.stage == slang::ShaderStage::Compute) {
                has_supported_entry_point = entry.compute_thread_group_size == array<u32, 3>{8u, 8u, 1u};
                break;
            }
        }
        if (!has_supported_entry_point) {
            cleanup();
            return unexpected(custom_compute_error(
                "Custom compute entry point must declare exactly [numthreads(8, 8, 1)]."));
        }

        const vector<GeneratedBindGroupLayout> generated =
            generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
        const vector<ReflectedResource> reflected_resources = collect_resource_bindings(reflection);
        if (generated.size() != 1 || generated.front().set != 0 || generated.front().entries.size() != 3 ||
            reflected_resources.size() != 3) {
            cleanup();
            return unexpected(custom_compute_error(
                "Custom compute shader must expose only set 0 with sourceTexture, sourceSampler, and outputTexture."));
        }

        for (const RHI::BindGroupLayoutEntry &entry : generated.front().entries) {
            if (entry.count != 1) {
                cleanup();
                return unexpected(custom_compute_error(
                    "Custom compute resources must be singular descriptors, not descriptor arrays."));
            }
        }

        bool has_source = false;
        bool has_sampler = false;
        bool has_output = false;
        for (const ReflectedResource &binding : reflected_resources) {
            if (binding.set != 0) {
                cleanup();
                return unexpected(custom_compute_error("Custom compute resources must all use descriptor set 0."));
            }
            if (binding.name == "sourceTexture" && binding.type == RHI::BindingType::SampledTexture) {
                resource.source_binding = binding.binding;
                has_source = true;
            } else if (binding.name == "sourceSampler" && binding.type == RHI::BindingType::Sampler) {
                resource.sampler_binding = binding.binding;
                has_sampler = true;
            } else if (binding.name == "outputTexture" && binding.type == RHI::BindingType::StorageTexture) {
                resource.output_binding = binding.binding;
                has_output = true;
            } else {
                cleanup();
                return unexpected(custom_compute_error(
                    "Custom compute resources must be exactly Texture2D sourceTexture, SamplerState sourceSampler, and RWTexture2D outputTexture."));
            }
        }
        if (!has_source || !has_sampler || !has_output) {
            cleanup();
            return unexpected(custom_compute_error(
                "Custom compute resources must be exactly Texture2D sourceTexture, SamplerState sourceSampler, and RWTexture2D outputTexture."));
        }

        const GeneratedBindGroupLayout &layout = generated.front();
        auto bind_group_layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{layout.entries.data(), layout.entries.size()},
            .label = "custom compute effect bind group layout",
        });
        if (!bind_group_layout) {
            cleanup();
            return unexpected(graphics_error_from_rhi(
                bind_group_layout.error(), "create custom compute effect bind group layout"));
        }
        resource.bind_group_layout = *bind_group_layout;

        const vector<RHI::PushConstantRange> push_ranges =
            generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
        if (push_ranges.size() > 1 || (!push_ranges.empty() && push_ranges.front().offset != 0)) {
            cleanup();
            return unexpected(custom_compute_error(
                "Custom compute effects support at most one push-constant range beginning at byte zero."));
        }
        resource.push_constant_size = push_ranges.empty() ? 0u : push_ranges.front().size;
        if (effect.push_constants.size() != resource.push_constant_size) {
            cleanup();
            return unexpected(custom_compute_error(
                "Custom compute push-constant payload size does not match the shader's reflected range."));
        }

        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{&resource.bind_group_layout, 1},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "custom compute effect pipeline layout",
        });
        if (!pipeline_layout) {
            cleanup();
            return unexpected(graphics_error_from_rhi(
                pipeline_layout.error(), "create custom compute effect pipeline layout"));
        }
        resource.pipeline_layout = *pipeline_layout;

        auto sampler = device->create_sampler(RHI::SamplerDesc{
            .min_filter = RHI::Filter::Linear,
            .mag_filter = RHI::Filter::Linear,
            .mipmap_mode = RHI::MipmapMode::Nearest,
            .address_u = RHI::AddressMode::ClampToEdge,
            .address_v = RHI::AddressMode::ClampToEdge,
            .address_w = RHI::AddressMode::ClampToEdge,
            .max_lod = 0.0f,
            .label = "custom compute effect sampler",
        });
        if (!sampler) {
            cleanup();
            return unexpected(graphics_error_from_rhi(sampler.error(), "create custom compute effect sampler"));
        }
        resource.sampler = *sampler;

        auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
            .layout = resource.pipeline_layout,
            .compute = RHI::ShaderEntry{
                .module = resource.module,
                .entry_point = resource.compute_entry_point.c_str(),
                .stage = RHI::ShaderStage::Compute,
            },
            .label = "custom compute effect pipeline",
        });
        if (!pipeline) {
            cleanup();
            return unexpected(graphics_error_from_rhi(pipeline.error(), "create custom compute effect pipeline"));
        }
        resource.pipeline = *pipeline;
        resource.shader.release_compiler_state();
        resources->push_back(std::move(resource));
        return {};
    }

    Core::RendererResult Renderer::record_custom_compute_effect(
        RHI::ComputePassEncoder &pass,
        RHI::TextureViewHandle source_view,
        RHI::TextureViewHandle output_view,
        Core::Extent2D extent,
        const CustomComputeEffect &effect,
        vector<RHI::BindGroupHandle> &transient_bind_groups) {
        if (Core::RendererResult ready = ensure_custom_compute_effect(effect); !ready.has_value()) {
            return ready;
        }
        if (!source_view || !output_view || extent.width == 0 || extent.height == 0) {
            return unexpected(custom_compute_error(
                "Cannot record a custom compute effect without valid source/output views and extent."));
        }

        auto resources = custom_compute_effect_resources_.lock();
        CustomComputeEffectResources *resource = nullptr;
        for (CustomComputeEffectResources &candidate : *resources) {
            if (candidate.shader_path == effect.shader_path && candidate.module_name == effect.module_name &&
                candidate.compute_entry_point == effect.compute_entry_point) {
                resource = &candidate;
                break;
            }
        }
        if (resource == nullptr) {
            return unexpected(custom_compute_error("Custom compute effect cache lookup failed."));
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(custom_compute_error("Cannot record a custom compute effect without an RHI device."));
        }
        const array<RHI::BindGroupEntry, 3> entries{
            RHI::BindGroupEntry{.binding = resource->source_binding, .texture_view = source_view},
            RHI::BindGroupEntry{.binding = resource->sampler_binding, .sampler = resource->sampler},
            RHI::BindGroupEntry{.binding = resource->output_binding, .texture_view = output_view},
        };
        auto group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = resource->bind_group_layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .label = "custom compute effect bind group",
        });
        if (!group) {
            return unexpected(graphics_error_from_rhi(group.error(), "create custom compute effect bind group"));
        }
        {
            auto guard = transient_bind_groups_lock_.lock();
            transient_bind_groups.push_back(*group);
        }

        pass.set_pipeline(resource->pipeline);
        pass.set_bind_group(0, *group);
        if (!effect.push_constants.empty()) {
            pass.set_push_constants(
                RHI::ShaderStage::Compute,
                0,
                span<const std::byte>{effect.push_constants.data(), effect.push_constants.size()});
        }
        pass.dispatch((extent.width + 7u) / 8u, (extent.height + 7u) / 8u, 1);
        return {};
    }

    void Renderer::destroy_custom_compute_effect_resources() noexcept {
        auto resources = custom_compute_effect_resources_.lock();
        RHI::RhiDevice *device = rhi_device();
        if (device != nullptr) {
            for (CustomComputeEffectResources &resource : *resources) {
                if (resource.pipeline) device->destroy_compute_pipeline(resource.pipeline);
                if (resource.sampler) device->destroy_sampler(resource.sampler);
                if (resource.pipeline_layout) device->destroy_pipeline_layout(resource.pipeline_layout);
                if (resource.bind_group_layout) device->destroy_bind_group_layout(resource.bind_group_layout);
                if (resource.module) device->destroy_shader_module(resource.module);
            }
        }
        resources->clear();
    }
} // namespace SFT::Renderer
