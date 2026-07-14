// RhiDevice sampler resource creation/destruction.
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <utility>
#pragma endregion

#include <Foundation/Foundation.hpp>

#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <Core/Vulkan/VulkanSampler.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::SamplerHandle> VulkanRhiDeviceBridge::create_sampler(const rhi::SamplerDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::SamplerHandle>("create_sampler");
        }

        // Anisotropy is a Vulkan device feature: enabling it on a sampler is only valid when the
        // feature was turned on at device creation (VulkanBackend enables it whenever supported).
        // The RHI documents max_anisotropy as ">1 requests that ratio, clamped to the device limit",
        // so honor that clamp here rather than passing an over-range value straight to a validation error.
        const bool anisotropy_available = physical_device_ != nullptr &&
                                          physical_device_->features().samplerAnisotropy == VK_TRUE;
        const bool use_anisotropy = anisotropy_available && desc.max_anisotropy > 1.0f;
        const f32 max_anisotropy = use_anisotropy
                                       ? std::min(desc.max_anisotropy,
                                                  physical_device_->properties().limits.maxSamplerAnisotropy)
                                       : 1.0f;

        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = to_vk(desc.mag_filter),
            .minFilter = to_vk(desc.min_filter),
            .mipmapMode = to_vk(desc.mipmap_mode),
            .addressModeU = to_vk(desc.address_u),
            .addressModeV = to_vk(desc.address_v),
            .addressModeW = to_vk(desc.address_w),
            .mipLodBias = desc.mip_lod_bias,
            .anisotropyEnable = use_anisotropy ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = max_anisotropy,
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
