#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanHelpers.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using std::nullopt;
using std::optional;
using std::string_view;
using std::vector;

namespace SFT::Core::Vulkan {

    // Wraps a VkPhysicalDevice. Properties, features, memory properties, and queue families
    // are queried and cached at construction time — they are immutable for the lifetime of the
    // VkInstance. Surface-dependent queries (capabilities, formats, present modes) are per-call
    // since they change as the surface or window is resized or recreated.
    class VulkanPhysicalDevice {
      public:
        VulkanPhysicalDevice() = default;

        explicit VulkanPhysicalDevice(VkPhysicalDevice device);

        // Enumerates every physical device visible to instance, wrapped and pre-queried.
        [[nodiscard]] static RendererExpected<vector<VulkanPhysicalDevice>> enumerate(VkInstance instance);

        [[nodiscard]] VkPhysicalDevice vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        [[nodiscard]] const VkPhysicalDeviceProperties &properties() const noexcept;
        [[nodiscard]] const VkPhysicalDeviceFeatures &features() const noexcept;

        // Populates the extended feature chain rooted at `features` (the VkPhysicalDeviceFeatures2
        // counterpart to features(), which exposes only the cached core 1.0 set). Chain the version
        // or extension feature structs you want to probe into features.pNext before calling; each is
        // filled in place. Requires Vulkan 1.1+ / VK_KHR_get_physical_device_properties2.
        void query_features2(VkPhysicalDeviceFeatures2 &features) const noexcept;
        [[nodiscard]] const VkPhysicalDeviceMemoryProperties &memory_properties() const noexcept;
        [[nodiscard]] const vector<VkQueueFamilyProperties> &queue_families() const noexcept;
        // `deviceName` is a fixed-size C array the driver fills and NUL-terminates. `from_c_str` scans
        // only within `VK_MAX_PHYSICAL_DEVICE_NAME_SIZE`, so a driver that forgot the terminator can't make
        // us over-read. Borrows the array (no copy), so the returned slice lives as long as this device.
        // Not `noexcept`: `ustr` validates UTF-8 and throws on a non-UTF-8 name (there is no non-throwing
        // borrowed constructor) — a real friction point versus the old `string_view` return.
        [[nodiscard]] ustr name() const;
        [[nodiscard]] const char *type_name() const noexcept;

        // PCI vendor ID and its readable name (AMD / NVIDIA / Intel / ...).
        [[nodiscard]] u32 vendor_id() const noexcept;
        [[nodiscard]] const char *vendor_name() const noexcept;
        [[nodiscard]] u32 device_id() const noexcept;

        // Raw, vendor-encoded driver version, plus a decoded human-readable form. The raw value's
        // bit layout differs per vendor — use driver_version_string() for anything user-facing.
        [[nodiscard]] u32 driver_version() const noexcept;
        [[nodiscard]] UString driver_version_string() const;

        // The Vulkan API version this device supports (standard VK_API_VERSION_* layout).
        [[nodiscard]] u32 api_version() const noexcept;
        [[nodiscard]] UString api_version_string() const;

        // Nanoseconds per GPU timestamp tick — multiply raw tick deltas by this value.
        [[nodiscard]] f32 timestamp_period() const noexcept;

        // Number of valid high bits in timestamp values written by queue family i.
        // 0 means the queue family does not support timestamps.
        [[nodiscard]] u32 timestamp_valid_bits(u32 queue_family_index) const noexcept;

        // Returns the time domains available for calibrated timestamp queries on this device.
        // Requires VK_KHR_calibrated_timestamps / Vulkan 1.4 core.
        [[nodiscard]] RendererExpected<vector<VkTimeDomainKHR>> calibrateable_time_domains() const noexcept;

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
        [[nodiscard]] optional<u32> findGraphicsQueue(VkSurfaceKHR surface) noexcept;

        // Returns the index of a queue family that can present to the given surface.
        [[nodiscard]] optional<u32> find_present_queue_family(VkSurfaceKHR surface) noexcept;

        [[nodiscard]] optional<u32> find_compute_queue_family() noexcept;

        [[nodiscard]] optional<u32> find_transfer_queue_family() noexcept;

        [[nodiscard]] optional<u32> find_sparse_binding_queue_family() noexcept;

        [[nodiscard]] optional<u32> find_protected_queue_family() noexcept;

        [[nodiscard]] optional<u32> find_video_decode_queue_family() noexcept;

        [[nodiscard]] optional<u32> find_video_encode_queue_family() noexcept;

        [[nodiscard]] optional<u32> find_optical_flow_queue_family() noexcept;

        // Every device extension this physical device advertises support for. Used to probe for
        // extensions that only exist on some backends (e.g. VK_KHR_portability_subset on MoltenVK)
        // instead of assuming a fixed vendor-specific set.
        [[nodiscard]] RendererExpected<vector<VkExtensionProperties>> enumerate_extensions() const;

        // Whether this device advertises support for a given device extension by name.
        [[nodiscard]] bool supports_extension(string_view name) const;

        // Surface capability queries used during swapchain creation and resize.
        [[nodiscard]] RendererExpected<VkSurfaceCapabilitiesKHR>
        surface_capabilities(VkSurfaceKHR surface) const noexcept;

        [[nodiscard]] RendererExpected<vector<VkSurfaceFormatKHR>>
        surface_formats(VkSurfaceKHR surface) const;

        [[nodiscard]] RendererExpected<vector<VkPresentModeKHR>>
        surface_present_modes(VkSurfaceKHR surface) const;

      private:
        // Index of the first queue family whose flags include every bit in `flags`, or nullopt.
        [[nodiscard]] optional<u32> find_queue_family_with(VkQueueFlags flags) const noexcept;

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
