// RhiDevice render (graphics) pipeline creation/destruction via GraphicsPipelineBuilder — always
// dynamic-rendering, matching the engine's no-VkRenderPass stance. Mesh-shader pipeline requests are
// rejected unless the device exposes the matching RHI features.
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanPipeline.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::RenderPipelineHandle> VulkanRhiDeviceBridge::create_render_pipeline(const rhi::RenderPipelineDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::RenderPipelineHandle>("create_render_pipeline");
        }
        const bool uses_mesh_shader = desc.mesh.module.is_valid() || desc.task.module.is_valid();
        if (uses_mesh_shader) {
            if (!desc.mesh.module.is_valid()) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_render_pipeline: a mesh shader entry is required when using the mesh pipeline path.");
            }
            if (!enabled_features_.has(rhi::Feature::MeshShader)) {
                return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                      "create_render_pipeline: mesh-shader pipelines require enabled Feature::MeshShader.");
            }
            if (desc.task.module.is_valid() && !enabled_features_.has(rhi::Feature::TaskShader)) {
                return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                      "create_render_pipeline: task/amplification shaders require enabled Feature::TaskShader.");
            }
        } else if (!desc.vertex.module.is_valid()) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_render_pipeline: a vertex shader entry is required.");
        }

        VulkanPipelineLayout *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_render_pipeline: unknown pipeline layout handle.");
        }

        GraphicsPipelineBuilder builder;
        builder.set_layout(layout->vk_handle());
        if (uses_mesh_shader) {
            builder.set_mesh_shader_frontend();
            if (desc.task.module.is_valid()) {
                VkShaderModule *task_module = shader_modules_.find(desc.task.module);
                if (task_module == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                          "create_render_pipeline: unknown task shader module handle.");
                }
                builder.add_stage(VkPipelineShaderStageCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_TASK_BIT_EXT,
                    .module = *task_module,
                    .pName = desc.task.entry_point,
                });
            }
            VkShaderModule *mesh_module = shader_modules_.find(desc.mesh.module);
            if (mesh_module == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_render_pipeline: unknown mesh shader module handle.");
            }
            builder.add_stage(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
                .module = *mesh_module,
                .pName = desc.mesh.entry_point,
            });
        } else {
            VkShaderModule *vertex_module = shader_modules_.find(desc.vertex.module);
            if (vertex_module == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_render_pipeline: unknown vertex shader module handle.");
            }
            builder.add_stage(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_VERTEX_BIT,
                .module = *vertex_module,
                .pName = desc.vertex.entry_point,
            });
        }

        if (desc.fragment.module.is_valid()) {
            VkShaderModule *fragment_module = shader_modules_.find(desc.fragment.module);
            if (fragment_module == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_render_pipeline: unknown fragment shader module handle.");
            }
            builder.add_stage(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = *fragment_module,
                .pName = desc.fragment.entry_point,
            });
        }

        vector<VkVertexInputBindingDescription> bindings;
        vector<VkVertexInputAttributeDescription> attributes;
        bindings.reserve(uses_mesh_shader ? 0 : desc.vertex_buffers.size());
        for (usize i = 0; !uses_mesh_shader && i < desc.vertex_buffers.size(); ++i) {
            const rhi::VertexBufferLayout &buffer_layout = desc.vertex_buffers[i];
            bindings.push_back(VkVertexInputBindingDescription{
                .binding = static_cast<u32>(i),
                .stride = static_cast<u32>(buffer_layout.stride),
                .inputRate = to_vk(buffer_layout.step_mode),
            });
            for (const rhi::VertexAttribute &attribute : buffer_layout.attributes) {
                attributes.push_back(VkVertexInputAttributeDescription{
                    .location = attribute.shader_location,
                    .binding = static_cast<u32>(i),
                    .format = to_vk(attribute.format),
                    .offset = attribute.offset,
                });
            }
        }
        builder.set_vertex_input(bindings, attributes);
        builder.set_topology(to_vk(desc.topology));

        builder.set_polygon_mode(to_vk(desc.rasterization.polygon_mode));
        builder.set_cull_mode(to_vk(desc.rasterization.cull_mode), to_vk(desc.rasterization.front_face));
        builder.set_depth_clamp(desc.rasterization.depth_clamp_enable);
        builder.set_line_width(desc.rasterization.line_width);
        if (desc.rasterization.depth_bias_constant != 0.0f || desc.rasterization.depth_bias_slope_scale != 0.0f ||
            desc.rasterization.depth_bias_clamp != 0.0f) {
            builder.set_depth_bias(desc.rasterization.depth_bias_constant, desc.rasterization.depth_bias_clamp,
                                   desc.rasterization.depth_bias_slope_scale);
        }

        builder.set_samples(to_vk(desc.multisample.samples), desc.multisample.alpha_to_coverage_enable);
        builder.set_sample_mask(desc.multisample.sample_mask);

        builder.set_depth_test(desc.depth_stencil.depth_test_enable, desc.depth_stencil.depth_write_enable,
                               to_vk(desc.depth_stencil.depth_compare));
        if (desc.depth_stencil.stencil_test_enable) {
            const VkStencilOpState front{
                .failOp = to_vk(desc.depth_stencil.stencil_front.fail_op),
                .passOp = to_vk(desc.depth_stencil.stencil_front.pass_op),
                .depthFailOp = to_vk(desc.depth_stencil.stencil_front.depth_fail_op),
                .compareOp = to_vk(desc.depth_stencil.stencil_front.compare),
                .compareMask = desc.depth_stencil.stencil_read_mask,
                .writeMask = desc.depth_stencil.stencil_write_mask,
                .reference = 0,
            };
            const VkStencilOpState back{
                .failOp = to_vk(desc.depth_stencil.stencil_back.fail_op),
                .passOp = to_vk(desc.depth_stencil.stencil_back.pass_op),
                .depthFailOp = to_vk(desc.depth_stencil.stencil_back.depth_fail_op),
                .compareOp = to_vk(desc.depth_stencil.stencil_back.compare),
                .compareMask = desc.depth_stencil.stencil_read_mask,
                .writeMask = desc.depth_stencil.stencil_write_mask,
                .reference = 0,
            };
            builder.set_stencil(front, back);
        }
        builder.set_depth_format(to_vk(desc.depth_stencil.format));

        for (const rhi::ColorTargetState &target : desc.color_targets) {
            const VkPipelineColorBlendAttachmentState blend{
                .blendEnable = target.blend_enable ? VK_TRUE : VK_FALSE,
                .srcColorBlendFactor = to_vk(target.color.src_factor),
                .dstColorBlendFactor = to_vk(target.color.dst_factor),
                .colorBlendOp = to_vk(target.color.op),
                .srcAlphaBlendFactor = to_vk(target.alpha.src_factor),
                .dstAlphaBlendFactor = to_vk(target.alpha.dst_factor),
                .alphaBlendOp = to_vk(target.alpha.op),
                .colorWriteMask = to_vk(target.write_mask),
            };
            builder.add_color_target(to_vk(target.format), blend);
        }

        builder.set_view_mask(desc.view_mask);

        auto pipeline = builder.create(logical_device_->vk_handle());
        if (!pipeline) {
            return rhi_error_from_graphics(pipeline.error());
        }
        return render_pipelines_.insert(PipelineRecord{std::move(*pipeline), desc.layout});
    }

    void VulkanRhiDeviceBridge::destroy_render_pipeline(rhi::RenderPipelineHandle handle) noexcept {
        render_pipelines_.erase(handle);
    }

} // namespace SFT::Core::Vulkan
