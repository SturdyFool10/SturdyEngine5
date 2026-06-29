module;
#include "volk.h"
#include <optional>
#include <string_view>
#include <vector>

export module Sturdy.Core:VulkanPhysicalDevice;

import :RendererError;
import :VulkanHelpers;
import Sturdy.Foundation;

using std::vector;
using std::optional;
using std::nullopt;
using std::string_view;
using SFT::Core::RendererExpected;
using SFT::Core::RendererErrorCode;
using SFT::Core::renderer_error;

export namespace SFT::Core::Vulkan {

// Wraps a VkPhysicalDevice. Properties, features, memory properties, and queue families
// are queried and cached at construction time — they are immutable for the lifetime of the
// VkInstance. Surface-dependent queries (capabilities, formats, present modes) are per-call
// since they change as the surface or window is resized or recreated.
class VulkanPhysicalDevice {
  public:
    VulkanPhysicalDevice() = default;

    explicit VulkanPhysicalDevice(VkPhysicalDevice device) : device_(device) {
        if (device_ == VK_NULL_HANDLE) return;
        vkGetPhysicalDeviceProperties(device_, &properties_);
        vkGetPhysicalDeviceFeatures(device_, &features_);
        vkGetPhysicalDeviceMemoryProperties(device_, &memory_properties_);

        u32 family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device_, &family_count, nullptr);
        queue_families_.resize(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device_, &family_count, queue_families_.data());
    }

    [[nodiscard]] VkPhysicalDevice vk_handle() const noexcept { return device_; }
    [[nodiscard]] bool is_valid() const noexcept { return device_ != VK_NULL_HANDLE; }

    [[nodiscard]] const VkPhysicalDeviceProperties &properties() const noexcept { return properties_; }
    [[nodiscard]] const VkPhysicalDeviceFeatures &features() const noexcept { return features_; }
    [[nodiscard]] const VkPhysicalDeviceMemoryProperties &memory_properties() const noexcept { return memory_properties_; }
    [[nodiscard]] const vector<VkQueueFamilyProperties> &queue_families() const noexcept { return queue_families_; }
    [[nodiscard]] string_view name() const noexcept { return properties_.deviceName; }
    [[nodiscard]] const char *type_name() const noexcept { return physical_device_type_name(properties_.deviceType); }

    // Heuristic score used during device selection. Higher is better.
    [[nodiscard]] f64 score() const noexcept {
        const auto &lim = properties_.limits;
        f64 s = (lim.maxFramebufferWidth / 1000.0) * (lim.maxFramebufferHeight / 1000.0);
        s += lim.maxPushConstantsSize / 16.0;
        switch (properties_.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   s *= 1.0; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: s *= 0.3; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    s *= 0.2; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            s *= 0.2; break;
            default:                                     s *= 0.1; break;
        }
        return s;
    }

    // Returns the index of a queue family that supports graphics commands.
    [[nodiscard]] optional<u32> findGraphicsQueue(VkSurfaceKHR surface) noexcept {
        if (gfxQueueFamIdx.has_value()) return gfxQueueFamIdx;
        u32 queueFamCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties2(this->device_, &queueFamCount, nullptr);
        vector<VkQueueFamilyProperties2> qfamprops(queueFamCount, {.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, .pNext = nullptr});
        vkGetPhysicalDeviceQueueFamilyProperties2(this->device_, &queueFamCount, qfamprops.data());
        for (u32 idx = 0; idx < queueFamCount; ++idx) {
            VkBool32 hasPresentSupp = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(this->device_, idx, surface, &hasPresentSupp);
            const auto &props = qfamprops[idx];
            if ((props.queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) && hasPresentSupp) {
                this->gfxQueueFamIdx = idx;
                return idx;
            }
        }
        return nullopt;
    }

    // Returns the index of a queue family that can present to the given surface.
    [[nodiscard]] optional<u32> find_present_queue_family(VkSurfaceKHR surface) noexcept {
        if (presentQueueFamIdx.has_value()) return presentQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(device_, i, surface, &supported) == VK_SUCCESS
                && supported) {
                this->presentQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    [[nodiscard]] optional<u32> find_compute_queue_family() noexcept {
        if (computeQueueFamIdx.has_value()) return computeQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            if (queue_families_[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                computeQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    [[nodiscard]] optional<u32> find_transfer_queue_family() noexcept {
        if (transferQueueFamIdx.has_value()) return transferQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            if (queue_families_[i].queueFlags & VK_QUEUE_TRANSFER_BIT) {
                transferQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    [[nodiscard]] optional<u32> find_sparse_binding_queue_family() noexcept {
        if (sparseBindingQueueFamIdx.has_value()) return sparseBindingQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            if (queue_families_[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) {
                sparseBindingQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    [[nodiscard]] optional<u32> find_protected_queue_family() noexcept {
        if (protectedQueueFamIdx.has_value()) return protectedQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            if (queue_families_[i].queueFlags & VK_QUEUE_PROTECTED_BIT) {
                protectedQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    [[nodiscard]] optional<u32> find_video_decode_queue_family() noexcept {
        if (videoDecodeQueueFamIdx.has_value()) return videoDecodeQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            if (queue_families_[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
                videoDecodeQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    [[nodiscard]] optional<u32> find_video_encode_queue_family() noexcept {
        if (videoEncodeQueueFamIdx.has_value()) return videoEncodeQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            if (queue_families_[i].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) {
                videoEncodeQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    [[nodiscard]] optional<u32> find_optical_flow_queue_family() noexcept {
        if (opticalFlowQueueFamIdx.has_value()) return opticalFlowQueueFamIdx;
        for (u32 i = 0; i < static_cast<u32>(queue_families_.size()); ++i) {
            if (queue_families_[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) {
                opticalFlowQueueFamIdx = i;
                return i;
            }
        }
        return nullopt;
    }

    // Surface capability queries used during swapchain creation and resize.
    [[nodiscard]] RendererExpected<VkSurfaceCapabilitiesKHR>
    surface_capabilities(VkSurfaceKHR surface) const noexcept {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_, surface, &caps) != VK_SUCCESS) {
            return renderer_error(RendererErrorCode::OperationFailed,
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
        }
        return caps;
    }

    [[nodiscard]] RendererExpected<vector<VkSurfaceFormatKHR>>
    surface_formats(VkSurfaceKHR surface) const {
        u32 count = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(device_, surface, &count, nullptr) != VK_SUCCESS
            || count == 0) {
            return renderer_error(RendererErrorCode::OperationFailed,
                "vkGetPhysicalDeviceSurfaceFormatsKHR failed or returned no formats.");
        }
        vector<VkSurfaceFormatKHR> formats(count);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(device_, surface, &count, formats.data()) != VK_SUCCESS) {
            return renderer_error(RendererErrorCode::OperationFailed,
                "vkGetPhysicalDeviceSurfaceFormatsKHR (populate) failed.");
        }
        return formats;
    }

    [[nodiscard]] RendererExpected<vector<VkPresentModeKHR>>
    surface_present_modes(VkSurfaceKHR surface) const {
        u32 count = 0;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(device_, surface, &count, nullptr) != VK_SUCCESS
            || count == 0) {
            return renderer_error(RendererErrorCode::OperationFailed,
                "vkGetPhysicalDeviceSurfacePresentModesKHR failed or returned no present modes.");
        }
        vector<VkPresentModeKHR> modes(count);
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(device_, surface, &count, modes.data()) != VK_SUCCESS) {
            return renderer_error(RendererErrorCode::OperationFailed,
                "vkGetPhysicalDeviceSurfacePresentModesKHR (populate) failed.");
        }
        return modes;
    }

  private:
    VkPhysicalDevice device_                           = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_             = {};
    VkPhysicalDeviceFeatures features_                 = {};
    VkPhysicalDeviceMemoryProperties memory_properties_ = {};
    vector<VkQueueFamilyProperties> queue_families_;
    optional<u32> gfxQueueFamIdx         = nullopt;
    optional<u32> presentQueueFamIdx     = nullopt;
    optional<u32> computeQueueFamIdx     = nullopt;
    optional<u32> transferQueueFamIdx    = nullopt;
    optional<u32> sparseBindingQueueFamIdx = nullopt;
    optional<u32> protectedQueueFamIdx   = nullopt;
    optional<u32> videoDecodeQueueFamIdx = nullopt;
    optional<u32> videoEncodeQueueFamIdx = nullopt;
    optional<u32> opticalFlowQueueFamIdx = nullopt;
};

} // namespace SFT::Core::Vulkan
