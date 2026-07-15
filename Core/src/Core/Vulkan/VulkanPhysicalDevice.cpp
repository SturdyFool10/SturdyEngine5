#include "VulkanPhysicalDevice.hpp"

namespace SFT::Core::Vulkan {

VulkanPhysicalDevice::VulkanPhysicalDevice(VkPhysicalDevice device) : device_(device) {
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

[[nodiscard]] RendererExpected<vector<VulkanPhysicalDevice>> VulkanPhysicalDevice::enumerate(VkInstance instance) {
            u32 count = 0;
            if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
                return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                      "No Vulkan-capable GPUs found on this system.");
            }
            vector<VkPhysicalDevice> raw(count);
            if (vkEnumeratePhysicalDevices(instance, &count, raw.data()) != VK_SUCCESS) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkEnumeratePhysicalDevices (populate) failed.");
            }
            return raw
                 | std::views::transform([](VkPhysicalDevice raw_device) { return VulkanPhysicalDevice(raw_device); })
                 | std::ranges::to<vector>();
        }

[[nodiscard]] VkPhysicalDevice VulkanPhysicalDevice::vk_handle() const noexcept { return device_; }

[[nodiscard]] bool VulkanPhysicalDevice::is_valid() const noexcept { return device_ != VK_NULL_HANDLE; }

[[nodiscard]] const VkPhysicalDeviceProperties &VulkanPhysicalDevice::properties() const noexcept { return properties_; }

[[nodiscard]] const VkPhysicalDeviceFeatures &VulkanPhysicalDevice::features() const noexcept { return features_; }

void VulkanPhysicalDevice::query_features2(VkPhysicalDeviceFeatures2 &features) const noexcept {
            vkGetPhysicalDeviceFeatures2(device_, &features);
        }

[[nodiscard]] const VkPhysicalDeviceMemoryProperties &VulkanPhysicalDevice::memory_properties() const noexcept { return memory_properties_; }

[[nodiscard]] const vector<VkQueueFamilyProperties> &VulkanPhysicalDevice::queue_families() const noexcept { return queue_families_; }

[[nodiscard]] ustr VulkanPhysicalDevice::name() const { return ustr::from_c_str(properties_.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE); }

[[nodiscard]] const char *VulkanPhysicalDevice::type_name() const noexcept { return physical_device_type_name(properties_.deviceType); }

[[nodiscard]] u32 VulkanPhysicalDevice::vendor_id() const noexcept { return properties_.vendorID; }

[[nodiscard]] const char *VulkanPhysicalDevice::vendor_name() const noexcept { return Vulkan::vendor_name(properties_.vendorID); }

[[nodiscard]] u32 VulkanPhysicalDevice::device_id() const noexcept { return properties_.deviceID; }

[[nodiscard]] u32 VulkanPhysicalDevice::driver_version() const noexcept { return properties_.driverVersion; }

[[nodiscard]] UString VulkanPhysicalDevice::driver_version_string() const {
            return format_driver_version(properties_.vendorID, properties_.driverVersion);
        }

[[nodiscard]] u32 VulkanPhysicalDevice::api_version() const noexcept { return properties_.apiVersion; }

[[nodiscard]] UString VulkanPhysicalDevice::api_version_string() const {
            return std::format("{}.{}.{}",
                               VK_API_VERSION_MAJOR(properties_.apiVersion),
                               VK_API_VERSION_MINOR(properties_.apiVersion),
                               VK_API_VERSION_PATCH(properties_.apiVersion));
        }

[[nodiscard]] f32 VulkanPhysicalDevice::timestamp_period() const noexcept {
            return properties_.limits.timestampPeriod;
        }

[[nodiscard]] u32 VulkanPhysicalDevice::timestamp_valid_bits(u32 queue_family_index) const noexcept {
            if (queue_family_index >= static_cast<u32>(queue_families_.size()))
                return 0;
            return queue_families_[queue_family_index].timestampValidBits;
        }

[[nodiscard]] RendererExpected<vector<VkTimeDomainKHR>> VulkanPhysicalDevice::calibrateable_time_domains() const noexcept {
            u32 count = 0;
            if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(device_, &count, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR (count) failed.");
            vector<VkTimeDomainKHR> domains(count);
            if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(device_, &count, domains.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR (populate) failed.");
            return domains;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::findGraphicsQueue(VkSurfaceKHR surface) noexcept {
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

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_present_queue_family(VkSurfaceKHR surface) noexcept {
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

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_compute_queue_family() noexcept {
            if (!computeQueueFamIdx.has_value())
                computeQueueFamIdx = find_queue_family_with(VK_QUEUE_COMPUTE_BIT);
            return computeQueueFamIdx;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_transfer_queue_family() noexcept {
            if (!transferQueueFamIdx.has_value())
                transferQueueFamIdx = find_queue_family_with(VK_QUEUE_TRANSFER_BIT);
            return transferQueueFamIdx;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_sparse_binding_queue_family() noexcept {
            if (!sparseBindingQueueFamIdx.has_value())
                sparseBindingQueueFamIdx = find_queue_family_with(VK_QUEUE_SPARSE_BINDING_BIT);
            return sparseBindingQueueFamIdx;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_protected_queue_family() noexcept {
            if (!protectedQueueFamIdx.has_value())
                protectedQueueFamIdx = find_queue_family_with(VK_QUEUE_PROTECTED_BIT);
            return protectedQueueFamIdx;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_video_decode_queue_family() noexcept {
            if (!videoDecodeQueueFamIdx.has_value())
                videoDecodeQueueFamIdx = find_queue_family_with(VK_QUEUE_VIDEO_DECODE_BIT_KHR);
            return videoDecodeQueueFamIdx;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_video_encode_queue_family() noexcept {
            if (!videoEncodeQueueFamIdx.has_value())
                videoEncodeQueueFamIdx = find_queue_family_with(VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
            return videoEncodeQueueFamIdx;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_optical_flow_queue_family() noexcept {
            if (!opticalFlowQueueFamIdx.has_value())
                opticalFlowQueueFamIdx = find_queue_family_with(VK_QUEUE_OPTICAL_FLOW_BIT_NV);
            return opticalFlowQueueFamIdx;
        }

[[nodiscard]] RendererExpected<vector<VkExtensionProperties>> VulkanPhysicalDevice::enumerate_extensions() const {
            u32 count = 0;
            if (vkEnumerateDeviceExtensionProperties(device_, nullptr, &count, nullptr) != VK_SUCCESS) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkEnumerateDeviceExtensionProperties (count) failed.");
            }
            vector<VkExtensionProperties> extensions(count);
            if (count > 0 && vkEnumerateDeviceExtensionProperties(device_, nullptr, &count, extensions.data()) != VK_SUCCESS) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkEnumerateDeviceExtensionProperties (populate) failed.");
            }
            return extensions;
        }

[[nodiscard]] bool VulkanPhysicalDevice::supports_extension(string_view name) const {
            auto extensions = enumerate_extensions();
            if (!extensions) return false;
            return std::ranges::any_of(*extensions, [&](const VkExtensionProperties &ext) {
                return name == string_view{ext.extensionName};
            });
        }

[[nodiscard]] RendererExpected<VkSurfaceCapabilitiesKHR>
        VulkanPhysicalDevice::surface_capabilities(VkSurfaceKHR surface) const noexcept {
            VkSurfaceCapabilitiesKHR caps{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_, surface, &caps) != VK_SUCCESS) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
            }
            return caps;
        }

[[nodiscard]] RendererExpected<vector<VkSurfaceFormatKHR>>
        VulkanPhysicalDevice::surface_formats(VkSurfaceKHR surface) const {
            u32 count = 0;
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(device_, surface, &count, nullptr) != VK_SUCCESS || count == 0) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceSurfaceFormatsKHR failed or returned no formats.");
            }
            vector<VkSurfaceFormatKHR> formats(count);
            if (vkGetPhysicalDeviceSurfaceFormatsKHR(device_, surface, &count, formats.data()) != VK_SUCCESS) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceSurfaceFormatsKHR (populate) failed.");
            }
            return formats;
        }

[[nodiscard]] RendererExpected<vector<VkPresentModeKHR>>
        VulkanPhysicalDevice::surface_present_modes(VkSurfaceKHR surface) const {
            u32 count = 0;
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(device_, surface, &count, nullptr) != VK_SUCCESS || count == 0) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceSurfacePresentModesKHR failed or returned no present modes.");
            }
            vector<VkPresentModeKHR> modes(count);
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(device_, surface, &count, modes.data()) != VK_SUCCESS) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceSurfacePresentModesKHR (populate) failed.");
            }
            return modes;
        }

[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_queue_family_with(VkQueueFlags flags) const noexcept {
            auto match = std::ranges::find_if(queue_families_, [flags](const VkQueueFamilyProperties &qf) {
                return (qf.queueFlags & flags) == flags;
            });
            if (match == queue_families_.end())
                return nullopt;
            return static_cast<u32>(match - queue_families_.begin());
        }

} // namespace SFT::Core::Vulkan
