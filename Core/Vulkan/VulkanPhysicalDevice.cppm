module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanPhysicalDevice;

#pragma region Imports
import :RendererError;
import :VulkanHelpers;
import Sturdy.Foundation;
#pragma endregion

using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using std::nullopt;
using std::optional;
using std::vector;

export namespace SFT::Core::Vulkan {

    // Wraps a VkPhysicalDevice. Properties, features, memory properties, and queue families
    // are queried and cached at construction time — they are immutable for the lifetime of the
    // VkInstance. Surface-dependent queries (capabilities, formats, present modes) are per-call
    // since they change as the surface or window is resized or recreated.
    class VulkanPhysicalDevice {
      public:
        VulkanPhysicalDevice() = default;

        explicit VulkanPhysicalDevice(VkPhysicalDevice device) : device_(device) {
            if (device_ == VK_NULL_HANDLE)
                return;
            vkGetPhysicalDeviceProperties(device_, &properties_);
            vkGetPhysicalDeviceFeatures(device_, &features_);
            vkGetPhysicalDeviceMemoryProperties(device_, &memory_properties_);

            u32 family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device_, &family_count, nullptr);
            queue_families_.resize(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(device_, &family_count, queue_families_.data());
        }

        // Enumerates every physical device visible to instance, wrapped and pre-queried.
        [[nodiscard]] static RendererExpected<vector<VulkanPhysicalDevice>> enumerate(VkInstance instance) {
            u32 count = 0;
            if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
                return renderer_error(RendererErrorCode::InitializationFailed,
                                      "No Vulkan-capable GPUs found on this system.");
            }
            vector<VkPhysicalDevice> raw(count);
            if (vkEnumeratePhysicalDevices(instance, &count, raw.data()) != VK_SUCCESS) {
                return renderer_error(RendererErrorCode::OperationFailed,
                                      "vkEnumeratePhysicalDevices (populate) failed.");
            }
            return raw
                 | std::views::transform([](VkPhysicalDevice raw_device) { return VulkanPhysicalDevice(raw_device); })
                 | std::ranges::to<vector>();
        }

        [[nodiscard]] VkPhysicalDevice vk_handle() const noexcept { return device_; }
        [[nodiscard]] bool is_valid() const noexcept { return device_ != VK_NULL_HANDLE; }

        [[nodiscard]] const VkPhysicalDeviceProperties &properties() const noexcept { return properties_; }
        [[nodiscard]] const VkPhysicalDeviceFeatures &features() const noexcept { return features_; }

        // Populates the extended feature chain rooted at `features` (the VkPhysicalDeviceFeatures2
        // counterpart to features(), which exposes only the cached core 1.0 set). Chain the version
        // or extension feature structs you want to probe into features.pNext before calling; each is
        // filled in place. Requires Vulkan 1.1+ / VK_KHR_get_physical_device_properties2.
        void query_features2(VkPhysicalDeviceFeatures2 &features) const noexcept {
            vkGetPhysicalDeviceFeatures2(device_, &features);
        }
        [[nodiscard]] const VkPhysicalDeviceMemoryProperties &memory_properties() const noexcept { return memory_properties_; }
        [[nodiscard]] const vector<VkQueueFamilyProperties> &queue_families() const noexcept { return queue_families_; }
        // `deviceName` is a fixed-size C array the driver fills and NUL-terminates. `from_c_str` scans
        // only within `VK_MAX_PHYSICAL_DEVICE_NAME_SIZE`, so a driver that forgot the terminator can't make
        // us over-read. Borrows the array (no copy), so the returned slice lives as long as this device.
        // Not `noexcept`: `ustr` validates UTF-8 and throws on a non-UTF-8 name (there is no non-throwing
        // borrowed constructor) — a real friction point versus the old `string_view` return.
        [[nodiscard]] ustr name() const { return ustr::from_c_str(properties_.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE); }
        [[nodiscard]] const char *type_name() const noexcept { return physical_device_type_name(properties_.deviceType); }

        // PCI vendor ID and its readable name (AMD / NVIDIA / Intel / ...).
        [[nodiscard]] u32 vendor_id() const noexcept { return properties_.vendorID; }
        [[nodiscard]] const char *vendor_name() const noexcept { return Vulkan::vendor_name(properties_.vendorID); }
        [[nodiscard]] u32 device_id() const noexcept { return properties_.deviceID; }

        // Raw, vendor-encoded driver version, plus a decoded human-readable form. The raw value's
        // bit layout differs per vendor — use driver_version_string() for anything user-facing.
        [[nodiscard]] u32 driver_version() const noexcept { return properties_.driverVersion; }
        [[nodiscard]] UString driver_version_string() const {
            return format_driver_version(properties_.vendorID, properties_.driverVersion);
        }

        // The Vulkan API version this device supports (standard VK_API_VERSION_* layout).
        [[nodiscard]] u32 api_version() const noexcept { return properties_.apiVersion; }
        [[nodiscard]] UString api_version_string() const {
            return std::format("{}.{}.{}",
                               VK_API_VERSION_MAJOR(properties_.apiVersion),
                               VK_API_VERSION_MINOR(properties_.apiVersion),
                               VK_API_VERSION_PATCH(properties_.apiVersion));
        }

        // Nanoseconds per GPU timestamp tick — multiply raw tick deltas by this value.
        [[nodiscard]] f32 timestamp_period() const noexcept {
            return properties_.limits.timestampPeriod;
        }

        // Number of valid high bits in timestamp values written by queue family i.
        // 0 means the queue family does not support timestamps.
        [[nodiscard]] u32 timestamp_valid_bits(u32 queue_family_index) const noexcept {
            if (queue_family_index >= static_cast<u32>(queue_families_.size()))
                return 0;
            return queue_families_[queue_family_index].timestampValidBits;
        }

        // Returns the time domains available for calibrated timestamp queries on this device.
        // Requires VK_KHR_calibrated_timestamps / Vulkan 1.4 core.
        [[nodiscard]] RendererExpected<vector<VkTimeDomainKHR>> calibrateable_time_domains() const noexcept {
            u32 count = 0;
            if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(device_, &count, nullptr) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR (count) failed.");
            vector<VkTimeDomainKHR> domains(count);
            if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(device_, &count, domains.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR (populate) failed.");
            return domains;
        }

        // Heuristic score used during device selection. Higher is better.
        [[nodiscard]] f64 score() const noexcept {
            const auto &lim = properties_.limits;
            f64 s = (lim.maxFramebufferWidth / 1000.0) * (lim.maxFramebufferHeight / 1000.0);
            s += lim.maxPushConstantsSize / 16.0;
            switch (properties_.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                    s *= 1.0;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                    s *= 0.3;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                    s *= 0.2;
                    break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU:
                    s *= 0.2;
                    break;
                default:
                    s *= 0.1;
                    break;
            }
            return s;
        }

        // Returns the index of a queue family that supports graphics commands.
        [[nodiscard]] optional<u32> findGraphicsQueue(VkSurfaceKHR surface) noexcept {
            if (gfxQueueFamIdx.has_value())
                return gfxQueueFamIdx;
            u32 queueFamCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties2(this->device_, &queueFamCount, nullptr);
            vector<VkQueueFamilyProperties2> qfamprops(queueFamCount, {.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, .pNext = nullptr});
            vkGetPhysicalDeviceQueueFamilyProperties2(this->device_, &queueFamCount, qfamprops.data());
            auto indices = std::views::iota(0u, queueFamCount);
            auto match = std::ranges::find_if(indices, [&](u32 idx) {
                VkBool32 hasPresentSupp = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(this->device_, idx, surface, &hasPresentSupp);
                return (qfamprops[idx].queueFamilyProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) && hasPresentSupp;
            });
            if (match == indices.end())
                return nullopt;
            this->gfxQueueFamIdx = *match;
            return gfxQueueFamIdx;
        }

        // Returns the index of a queue family that can present to the given surface.
        [[nodiscard]] optional<u32> find_present_queue_family(VkSurfaceKHR surface) noexcept {
            if (presentQueueFamIdx.has_value())
                return presentQueueFamIdx;
            auto indices = std::views::iota(0u, static_cast<u32>(queue_families_.size()));
            auto match = std::ranges::find_if(indices, [&](u32 i) {
                VkBool32 supported = VK_FALSE;
                return vkGetPhysicalDeviceSurfaceSupportKHR(device_, i, surface, &supported) == VK_SUCCESS && supported;
            });
            if (match == indices.end())
                return nullopt;
            this->presentQueueFamIdx = *match;
            return presentQueueFamIdx;
        }

        [[nodiscard]] optional<u32> find_compute_queue_family() noexcept {
            if (!computeQueueFamIdx.has_value())
                computeQueueFamIdx = find_queue_family_with(VK_QUEUE_COMPUTE_BIT);
            return computeQueueFamIdx;
        }

        [[nodiscard]] optional<u32> find_transfer_queue_family() noexcept {
            if (!transferQueueFamIdx.has_value())
                transferQueueFamIdx = find_queue_family_with(VK_QUEUE_TRANSFER_BIT);
            return transferQueueFamIdx;
        }

        [[nodiscard]] optional<u32> find_sparse_binding_queue_family() noexcept {
            if (!sparseBindingQueueFamIdx.has_value())
                sparseBindingQueueFamIdx = find_queue_family_with(VK_QUEUE_SPARSE_BINDING_BIT);
            return sparseBindingQueueFamIdx;
        }

        [[nodiscard]] optional<u32> find_protected_queue_family() noexcept {
            if (!protectedQueueFamIdx.has_value())
                protectedQueueFamIdx = find_queue_family_with(VK_QUEUE_PROTECTED_BIT);
            return protectedQueueFamIdx;
        }

        [[nodiscard]] optional<u32> find_video_decode_queue_family() noexcept {
            if (!videoDecodeQueueFamIdx.has_value())
                videoDecodeQueueFamIdx = find_queue_family_with(VK_QUEUE_VIDEO_DECODE_BIT_KHR);
            return videoDecodeQueueFamIdx;
        }

        [[nodiscard]] optional<u32> find_video_encode_queue_family() noexcept {
            if (!videoEncodeQueueFamIdx.has_value())
                videoEncodeQueueFamIdx = find_queue_family_with(VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
            return videoEncodeQueueFamIdx;
        }

        [[nodiscard]] optional<u32> find_optical_flow_queue_family() noexcept {
            if (!opticalFlowQueueFamIdx.has_value())
                opticalFlowQueueFamIdx = find_queue_family_with(VK_QUEUE_OPTICAL_FLOW_BIT_NV);
            return opticalFlowQueueFamIdx;
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
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(device_, surface, &count, nullptr) != VK_SUCCESS || count == 0) {
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
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(device_, surface, &count, nullptr) != VK_SUCCESS || count == 0) {
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
        // Index of the first queue family whose flags include every bit in `flags`, or nullopt.
        [[nodiscard]] optional<u32> find_queue_family_with(VkQueueFlags flags) const noexcept {
            auto match = std::ranges::find_if(queue_families_, [flags](const VkQueueFamilyProperties &qf) {
                return (qf.queueFlags & flags) == flags;
            });
            if (match == queue_families_.end())
                return nullopt;
            return static_cast<u32>(match - queue_families_.begin());
        }

        VkPhysicalDevice device_ = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties_ = {};
        VkPhysicalDeviceFeatures features_ = {};
        VkPhysicalDeviceMemoryProperties memory_properties_ = {};
        vector<VkQueueFamilyProperties> queue_families_;
        optional<u32> gfxQueueFamIdx = nullopt;
        optional<u32> presentQueueFamIdx = nullopt;
        optional<u32> computeQueueFamIdx = nullopt;
        optional<u32> transferQueueFamIdx = nullopt;
        optional<u32> sparseBindingQueueFamIdx = nullopt;
        optional<u32> protectedQueueFamIdx = nullopt;
        optional<u32> videoDecodeQueueFamIdx = nullopt;
        optional<u32> videoEncodeQueueFamIdx = nullopt;
        optional<u32> opticalFlowQueueFamIdx = nullopt;
    };

} // namespace SFT::Core::Vulkan
