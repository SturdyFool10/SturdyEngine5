#include <Foundation/Foundation.hpp>

#include <Renderer/ShaderTarget.hpp>

#include <algorithm>
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

using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Renderer {
    namespace {
        [[nodiscard]] Core::GraphicsBackendError compute_kernel_error(string message) {
            return Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed, std::move(message)};
        }
    } // namespace

    Core::RendererExpected<ComputeKernelId> Renderer::prepare_compute_kernel(const ComputeKernelDescription &kernel) {
        ZoneScopedN("Renderer::prepare_compute_kernel");
        if (kernel.shader_path.empty() || kernel.module_name.empty() || kernel.entry_point.empty()) {
            return unexpected(compute_kernel_error("A compute kernel requires shader_path, module_name and entry_point."));
        }
        auto kernels = compute_kernels_.lock();
        for (usize index = 0; index < kernels->size(); ++index) {
            const ComputeKernelResource &existing = (*kernels)[index];
            if (existing.shader_path == kernel.shader_path && existing.module_name == kernel.module_name &&
                existing.entry_point == kernel.entry_point) {
                return ComputeKernelId{static_cast<u32>(index)};
            }
        }

        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(compute_kernel_error("Cannot build a compute kernel without an RHI device."));
        }
        const string context = "compute kernel '" + kernel.module_name + "::" + kernel.entry_point + "'";

        ComputeKernelResource resource{
            .shader_path = kernel.shader_path,
            .module_name = kernel.module_name,
            .entry_point = kernel.entry_point,
        };
        auto cleanup = [&]() noexcept {
            if (resource.pipeline) device->destroy_compute_pipeline(resource.pipeline);
            if (resource.sampler) device->destroy_sampler(resource.sampler);
            if (resource.pipeline_layout) device->destroy_pipeline_layout(resource.pipeline_layout);
            if (resource.bind_group_layout) device->destroy_bind_group_layout(resource.bind_group_layout);
            if (resource.module) device->destroy_shader_module(resource.module);
        };

        const auto shader_target = shader_target_for_device(*device);
        if (!shader_target) return unexpected(shader_target.error());

        const slang::ShaderCompileOptions options{
            .targets = shader_compile_targets_for_device(device),
            .entry_points = {slang::ShaderEntryPointRequest{.name = kernel.entry_point, .stage = slang::ShaderStage::Compute}},
        };
        slang::ShaderVariantCache shader_cache{
            slang::ShaderSource::from_file(kernel.shader_path, kernel.module_name), options, slang::ShaderCompiler{},
            recovery_create_info_.enable_shader_disk_cache};
        auto shader = shader_cache.get_or_compile_base();
        if (!shader) {
            return unexpected(compute_kernel_error("compile " + context + " failed: " + shader.error().message + "\n" +
                                                   shader.error().diagnostics));
        }
        resource.shader = *shader;

        auto code = resource.shader.entry_point_code(kernel.entry_point, shader_target->slang_target.format);
        if (!code) {
            return unexpected(compute_kernel_error("generate bytecode for " + context + " failed: " + code.error().message));
        }
        const string module_label = kernel.label.empty() ? context : kernel.label;
        auto module = device->create_shader_module(RHI::ShaderModuleDesc{
            .language = shader_target->module_language,
            .code = span<const std::byte>{code->bytes.data(), code->bytes.size()},
            .label = module_label.c_str(),
        });
        if (!module) {
            return unexpected(graphics_error_from_rhi(module.error(), ("create shader module for " + context).c_str()));
        }
        resource.module = *module;

        const slang::ShaderReflection &reflection = resource.shader.reflection();
        const vector<GeneratedBindGroupLayout> generated = generate_bind_group_layouts(reflection, RHI::ShaderStage::Compute);
        if (generated.empty() || generated.front().set != 0) {
            cleanup();
            return unexpected(compute_kernel_error(context + " must declare its resources in descriptor set 0."));
        }
        for (const RHI::BindGroupLayoutEntry &entry : generated.front().entries) {
            if (entry.count != 1) {
                cleanup();
                return unexpected(compute_kernel_error(context + " resources must be singular descriptors, not arrays."));
            }
        }
        for (const ReflectedResource &binding : collect_resource_bindings(reflection)) {
            if (binding.set != 0) {
                continue;
            }
            if (binding.type != RHI::BindingType::SampledTexture && binding.type != RHI::BindingType::StorageTexture &&
                binding.type != RHI::BindingType::Sampler) {
                cleanup();
                return unexpected(compute_kernel_error(context + " may only declare textures and samplers in set 0; '" +
                                                       binding.name + "' is something else."));
            }
            resource.bindings.push_back(ComputeKernelResource::Binding{binding.name, binding.binding, binding.type});
        }

        auto bind_group_layout = device->create_bind_group_layout(RHI::BindGroupLayoutDesc{
            .entries = span<const RHI::BindGroupLayoutEntry>{generated.front().entries.data(), generated.front().entries.size()},
            .label = "compute kernel bind group layout",
        });
        if (!bind_group_layout) {
            cleanup();
            return unexpected(graphics_error_from_rhi(bind_group_layout.error(), ("create bind group layout for " + context).c_str()));
        }
        resource.bind_group_layout = *bind_group_layout;

        const vector<RHI::PushConstantRange> push_ranges = generate_push_constant_ranges(reflection, RHI::ShaderStage::Compute);
        if (push_ranges.size() > 1 || (!push_ranges.empty() && push_ranges.front().offset != 0)) {
            cleanup();
            return unexpected(compute_kernel_error(context + " supports at most one push-constant range beginning at byte zero."));
        }
        resource.push_constant_size = push_ranges.empty() ? 0u : push_ranges.front().size;

        auto pipeline_layout = device->create_pipeline_layout(RHI::PipelineLayoutDesc{
            .bind_group_layouts = span<const RHI::BindGroupLayoutHandle>{&resource.bind_group_layout, 1},
            .push_constant_ranges = span<const RHI::PushConstantRange>{push_ranges.data(), push_ranges.size()},
            .label = "compute kernel pipeline layout",
        });
        if (!pipeline_layout) {
            cleanup();
            return unexpected(graphics_error_from_rhi(pipeline_layout.error(), ("create pipeline layout for " + context).c_str()));
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
            .label = "compute kernel sampler",
        });
        if (!sampler) {
            cleanup();
            return unexpected(graphics_error_from_rhi(sampler.error(), ("create sampler for " + context).c_str()));
        }
        resource.sampler = *sampler;

        auto pipeline = device->create_compute_pipeline(RHI::ComputePipelineDesc{
            .layout = resource.pipeline_layout,
            .compute = RHI::ShaderEntry{
                .module = resource.module, .entry_point = resource.entry_point.c_str(), .stage = RHI::ShaderStage::Compute},
            .label = module_label.c_str(),
        });
        if (!pipeline) {
            cleanup();
            return unexpected(graphics_error_from_rhi(pipeline.error(), ("create pipeline for " + context).c_str()));
        }
        resource.pipeline = *pipeline;
        resource.shader.release_compiler_state();
        kernels->push_back(std::move(resource));
        return ComputeKernelId{static_cast<u32>(kernels->size() - 1)};
    }

    Core::RendererResult Renderer::record_compute_kernel(RHI::ComputePassEncoder &pass, ComputeKernelId kernel,
                                                         span<const ComputeBinding> bindings, span<const std::byte> push_constants,
                                                         glm::uvec3 groups, vector<RHI::BindGroupHandle> &transient_bind_groups) {
        ZoneScopedN("Renderer::record_compute_kernel");
        RHI::RhiDevice *device = rhi_device();
        if (device == nullptr) {
            return unexpected(compute_kernel_error("Cannot record a compute kernel without an RHI device."));
        }
        RHI::BindGroupLayoutHandle layout{};
        RHI::ComputePipelineHandle pipeline{};
        RHI::SamplerHandle sampler{};
        u32 push_constant_size = 0;
        vector<ComputeKernelResource::Binding> declared;
        string name;
        {
            auto kernels = compute_kernels_.lock();
            if (!kernel || kernel.index >= kernels->size()) {
                return unexpected(compute_kernel_error("Compute kernel handle is not valid (was it prepared?)."));
            }
            const ComputeKernelResource &resource = (*kernels)[kernel.index];
            layout = resource.bind_group_layout;
            pipeline = resource.pipeline;
            sampler = resource.sampler;
            push_constant_size = resource.push_constant_size;
            declared = resource.bindings;
            name = resource.module_name + "::" + resource.entry_point;
        }
        if (push_constants.size() > push_constant_size) {
            return unexpected(compute_kernel_error("Compute kernel '" + name + "' has a " + std::to_string(push_constant_size) +
                                                   "-byte push-constant block but was given " + std::to_string(push_constants.size()) +
                                                   " bytes."));
        }
        // The reflected block can be larger than the struct (trailing alignment padding); the tail is zero.
        vector<std::byte> padded_push_constants;
        if (push_constants.size() < push_constant_size) {
            padded_push_constants.assign(push_constant_size, std::byte{0});
            std::copy(push_constants.begin(), push_constants.end(), padded_push_constants.begin());
            push_constants = span<const std::byte>{padded_push_constants.data(), padded_push_constants.size()};
        }
        vector<RHI::BindGroupEntry> entries;
        entries.reserve(declared.size());
        for (const ComputeKernelResource::Binding &binding : declared) {
            if (binding.type == RHI::BindingType::Sampler) {
                entries.push_back(RHI::BindGroupEntry{.binding = binding.binding, .sampler = sampler});
                continue;
            }
            RHI::TextureViewHandle view{};
            for (const ComputeBinding &supplied : bindings) {
                if (supplied.name == binding.name) {
                    view = supplied.view;
                    break;
                }
            }
            if (!view) {
                return unexpected(compute_kernel_error("Compute kernel '" + name + "' was not given a texture for '" +
                                                       binding.name + "'."));
            }
            entries.push_back(RHI::BindGroupEntry{.binding = binding.binding, .texture_view = view});
        }
        auto bind_group = device->create_bind_group(RHI::BindGroupDesc{
            .layout = layout,
            .entries = span<const RHI::BindGroupEntry>{entries.data(), entries.size()},
            .lifetime = RHI::BindGroupLifetime::FrameTransient,
            .label = "compute kernel bind group",
        });
        if (!bind_group) {
            return unexpected(graphics_error_from_rhi(bind_group.error(), ("create bind group for compute kernel '" + name + "'").c_str()));
        }
        {
            auto guard = transient_bind_groups_lock_.lock();
            transient_bind_groups.push_back(*bind_group);
        }
        pass.set_pipeline(pipeline);
        pass.set_bind_group(0, *bind_group);
        if (push_constant_size != 0) {
            pass.set_push_constants(RHI::ShaderStage::Compute, 0, push_constants);
        }
        pass.dispatch(groups.x, groups.y, groups.z);
        return {};
    }

    void Renderer::destroy_compute_kernels() noexcept {
        ZoneScopedN("Renderer::destroy_compute_kernels");
        auto kernels = compute_kernels_.lock();
        if (RHI::RhiDevice *device = rhi_device()) {
            for (ComputeKernelResource &resource : *kernels) {
                if (resource.pipeline) device->destroy_compute_pipeline(resource.pipeline);
                if (resource.sampler) device->destroy_sampler(resource.sampler);
                if (resource.pipeline_layout) device->destroy_pipeline_layout(resource.pipeline_layout);
                if (resource.bind_group_layout) device->destroy_bind_group_layout(resource.bind_group_layout);
                if (resource.module) device->destroy_shader_module(resource.module);
            }
        }
        kernels->clear();
    }

} // namespace SFT::Renderer
