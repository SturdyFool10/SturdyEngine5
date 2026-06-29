module;
#include "volk.h"
#include <span>

export module Sturdy.Core:VulkanShaderModule;

import :RendererError;
import Sturdy.Foundation;

using std::span;
using SFT::Core::RendererExpected;
using SFT::Core::RendererErrorCode;
using SFT::Core::renderer_error;

export namespace SFT::Core::Vulkan {

class VulkanShaderModule {
  public:
    VulkanShaderModule() = default;
    ~VulkanShaderModule() { destroy(); }

    VulkanShaderModule(const VulkanShaderModule&)            = delete;
    VulkanShaderModule& operator=(const VulkanShaderModule&) = delete;

    VulkanShaderModule(VulkanShaderModule&& o) noexcept
        : device_(o.device_), module_(o.module_) {
        o.device_ = VK_NULL_HANDLE;
        o.module_ = VK_NULL_HANDLE;
    }
    VulkanShaderModule& operator=(VulkanShaderModule&& o) noexcept {
        if (this != &o) { destroy(); device_ = o.device_; module_ = o.module_;
            o.device_ = VK_NULL_HANDLE; o.module_ = VK_NULL_HANDLE; }
        return *this;
    }

    // Takes SPIR-V as a span of 32-bit words (the natural format from glslc / slangc).
    [[nodiscard]] static RendererExpected<VulkanShaderModule> create(
        VkDevice device, span<const u32> spirv
    ) noexcept {
        VkShaderModuleCreateInfo info{
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext    = nullptr,
            .flags    = 0,
            .codeSize = spirv.size_bytes(),
            .pCode    = spirv.data(),
        };
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS)
            return renderer_error(RendererErrorCode::OperationFailed, "vkCreateShaderModule failed.");
        VulkanShaderModule out;
        out.device_ = device;
        out.module_ = mod;
        return out;
    }

    // Convenience: build the pipeline stage create info for this module.
    [[nodiscard]] VkPipelineShaderStageCreateInfo stage_info(
        VkShaderStageFlagBits stage, const char* entry_point = "main",
        const VkSpecializationInfo* specialization = nullptr
    ) const noexcept {
        return {
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext               = nullptr,
            .flags               = 0,
            .stage               = stage,
            .module              = module_,
            .pName               = entry_point,
            .pSpecializationInfo = specialization,
        };
    }

    [[nodiscard]] VkShaderModule vk_handle() const noexcept { return module_; }
    [[nodiscard]] bool           is_valid()  const noexcept { return module_ != VK_NULL_HANDLE; }

    void destroy() noexcept {
        if (module_ == VK_NULL_HANDLE) return;
        vkDestroyShaderModule(device_, module_, nullptr);
        module_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
    }

  private:
    VkDevice       device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
};

} // namespace SFT::Core::Vulkan
