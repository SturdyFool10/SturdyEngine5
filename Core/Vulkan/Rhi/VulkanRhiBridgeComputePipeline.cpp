// RhiDevice compute pipeline creation/destruction.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <utility>
#pragma endregion

module Sturdy.Core;

import :VulkanDevice;
import :VulkanPipeline;
import :VulkanRhiBridge;
import Sturdy.Foundation;
import Sturdy.RHI;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::ComputePipelineHandle> VulkanRhiDeviceBridge::create_compute_pipeline(
        const rhi::ComputePipelineDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::ComputePipelineHandle>("create_compute_pipeline");
        }
        if (!desc.compute.module.is_valid()) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_compute_pipeline: a compute shader entry is required.");
        }

        VulkanPipelineLayout *layout = pipeline_layouts_.find(desc.layout);
        if (layout == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_compute_pipeline: unknown pipeline layout handle.");
        }
        VkShaderModule *module = shader_modules_.find(desc.compute.module);
        if (module == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_compute_pipeline: unknown compute shader module handle.");
        }

        const VkPipelineShaderStageCreateInfo stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *module,
            .pName = desc.compute.entry_point,
        };
        const VkComputePipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = stage,
            .layout = layout->vk_handle(),
        };

        auto pipeline = VulkanPipeline::create_compute(logical_device_->vk_handle(), VK_NULL_HANDLE, info);
        if (!pipeline) {
            return rhi_error_from_graphics(pipeline.error());
        }
        return compute_pipelines_.insert(PipelineRecord{std::move(*pipeline), desc.layout});
    }

    void VulkanRhiDeviceBridge::destroy_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept {
        compute_pipelines_.erase(handle);
    }

} // namespace SFT::Core::Vulkan
