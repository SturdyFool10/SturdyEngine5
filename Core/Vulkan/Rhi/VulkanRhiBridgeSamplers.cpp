// RhiDevice sampler resource creation/destruction.
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
import :VulkanRhiBridge;
import :VulkanRhiConvert;
import :VulkanSampler;
import Sturdy.Foundation;
import Sturdy.RHI;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::SamplerHandle> VulkanRhiDeviceBridge::create_sampler(const rhi::SamplerDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::SamplerHandle>("create_sampler");
        }

        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = to_vk(desc.mag_filter),
            .minFilter = to_vk(desc.min_filter),
            .mipmapMode = to_vk(desc.mipmap_mode),
            .addressModeU = to_vk(desc.address_u),
            .addressModeV = to_vk(desc.address_v),
            .addressModeW = to_vk(desc.address_w),
            .mipLodBias = desc.mip_lod_bias,
            .anisotropyEnable = desc.max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = desc.max_anisotropy,
            .compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE,
            .compareOp = to_vk(desc.compare),
            .minLod = desc.min_lod,
            .maxLod = desc.max_lod,
            .borderColor = to_vk(desc.border_color),
            .unnormalizedCoordinates = VK_FALSE,
        };

        auto sampler = VulkanSampler::create(logical_device_->vk_handle(), info);
        if (!sampler) {
            return rhi_error_from_graphics(sampler.error());
        }
        return samplers_.insert(std::move(*sampler));
    }

    void VulkanRhiDeviceBridge::destroy_sampler(rhi::SamplerHandle handle) noexcept {
        samplers_.erase(handle);
    }

} // namespace SFT::Core::Vulkan
