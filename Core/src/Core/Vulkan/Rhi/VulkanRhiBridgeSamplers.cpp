
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

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    /// Creates a sampler from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SamplerHandle> VulkanRhiDeviceBridge::create_sampler(const rhi::SamplerDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_sampler");
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::SamplerHandle>("create_sampler");
        }


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

    /// Destroys the sampler identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_sampler(rhi::SamplerHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_sampler");
        samplers_.erase(handle);
    }

} // namespace SFT::Core::Vulkan
