// RhiDevice shader-module resource creation/destruction. Takes raw SPIR-V bytecode straight through
// VulkanDevice::create_shader_module — the Slang-reflection-carrying VulkanShaderModule wrapper the
// demo render path uses is a different, source-file-keyed concern and not what this generic RHI entry
// point is for.
module;
#include <Foundation/Foundation.hpp>
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
import Sturdy.RHI;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    rhi::RhiExpected<rhi::ShaderModuleHandle> VulkanRhiDeviceBridge::create_shader_module(const rhi::ShaderModuleDesc &desc) {
        if (logical_device_ == nullptr) {
            return device_not_ready<rhi::ShaderModuleHandle>("create_shader_module");
        }
        if (desc.language != rhi::ShaderLanguage::SpirV) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "Vulkan RHI bridge only accepts ShaderLanguage::SpirV shader modules.");
        }
        if (desc.code.size() % 4 != 0) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "create_shader_module: SPIR-V code size must be a multiple of 4 bytes.");
        }

        const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = desc.code.size(),
            .pCode = reinterpret_cast<const u32 *>(desc.code.data()),
        };

        auto module = logical_device_->create_shader_module(info);
        if (!module) {
            return rhi_error_from_graphics(module.error());
        }
        return shader_modules_.insert(std::move(*module));
    }

    void VulkanRhiDeviceBridge::destroy_shader_module(rhi::ShaderModuleHandle handle) noexcept {
        if (VkShaderModule *module = shader_modules_.find(handle)) {
            if (logical_device_ != nullptr) {
                logical_device_->destroy_shader_module(*module);
            }
            shader_modules_.erase(handle);
        }
    }

} // namespace SFT::Core::Vulkan
