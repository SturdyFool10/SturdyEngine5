#include <Core/Vulkan/VulkanPhysicalDevice.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Performs the vulkan physical device operation for `Vulkan` using the supplied arguments.
///
/// @param device Device used or affected by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
VulkanPhysicalDevice::VulkanPhysicalDevice(VkPhysicalDevice device) : device_(device) {
            ZoneScopedN("VulkanPhysicalDevice::VulkanPhysicalDevice");
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

/// Enumerates the supplied or associated value/state using the supplied arguments and current state.
///
/// @param instance Instance used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`, `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<VulkanPhysicalDevice>> VulkanPhysicalDevice::enumerate(VkInstance instance) {
            ZoneScopedN("VulkanPhysicalDevice::enumerate");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPhysicalDevice VulkanPhysicalDevice::vk_handle() const noexcept { return device_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPhysicalDevice::is_valid() const noexcept { return device_ != VK_NULL_HANDLE; }

/// Returns the current or globally available properties value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VkPhysicalDeviceProperties &VulkanPhysicalDevice::properties() const noexcept { return properties_; }

/// Returns the current or globally available features value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VkPhysicalDeviceFeatures &VulkanPhysicalDevice::features() const noexcept { return features_; }

/// Queries features2 from the active backend or runtime state.
///
/// @param features `features` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanPhysicalDevice::query_features2(VkPhysicalDeviceFeatures2 &features) const noexcept {
            ZoneScopedN("VulkanPhysicalDevice::query_features2");
            vkGetPhysicalDeviceFeatures2(device_, &features);
        }

/// Returns the current or globally available memory properties value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const VkPhysicalDeviceMemoryProperties &VulkanPhysicalDevice::memory_properties() const noexcept { return memory_properties_; }

/// Returns the current or globally available queue families value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const vector<VkQueueFamilyProperties> &VulkanPhysicalDevice::queue_families() const noexcept { return queue_families_; }

/// Returns the current or globally available name value.
///
/// @return Returns the current name value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] ustr VulkanPhysicalDevice::name() const { return ustr::from_c_str(properties_.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE); }

/// Returns a human-readable name for the supplied type value.
///
/// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
/// @note This function does not throw exceptions.
[[nodiscard]] const char *VulkanPhysicalDevice::type_name() const noexcept { return physical_device_type_name(properties_.deviceType); }

/// Returns the current or globally available vendor ID value.
///
/// @return Returns the current vendor ID value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanPhysicalDevice::vendor_id() const noexcept { return properties_.vendorID; }

/// Returns a human-readable name for the supplied vendor value.
///
/// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
/// @note This function does not throw exceptions.
[[nodiscard]] const char *VulkanPhysicalDevice::vendor_name() const noexcept { return Vulkan::vendor_name(properties_.vendorID); }

/// Returns the current or globally available device ID value.
///
/// @return Returns the current device ID value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanPhysicalDevice::device_id() const noexcept { return properties_.deviceID; }

/// Returns the current or globally available driver version value.
///
/// @return Returns the current driver version value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanPhysicalDevice::driver_version() const noexcept { return properties_.driverVersion; }

/// Returns the current or globally available driver version string value.
///
/// @return Returns the current driver version string value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] UString VulkanPhysicalDevice::driver_version_string() const {
            ZoneScopedN("VulkanPhysicalDevice::driver_version_string");
            return format_driver_version(properties_.vendorID, properties_.driverVersion);
        }

/// Returns the current or globally available API version value.
///
/// @return Returns the current API version value.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanPhysicalDevice::api_version() const noexcept { return properties_.apiVersion; }

/// Returns the current or globally available API version string value.
///
/// @return Returns the current API version string value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] UString VulkanPhysicalDevice::api_version_string() const {
            ZoneScopedN("VulkanPhysicalDevice::api_version_string");
            return std::format("{}.{}.{}",
                               VK_API_VERSION_MAJOR(properties_.apiVersion),
                               VK_API_VERSION_MINOR(properties_.apiVersion),
                               VK_API_VERSION_PATCH(properties_.apiVersion));
        }

/// Returns the current or globally available timestamp period value.
///
/// @return Returns the current timestamp period value.
/// @note This function does not throw exceptions.
[[nodiscard]] f32 VulkanPhysicalDevice::timestamp_period() const noexcept {
            ZoneScopedN("VulkanPhysicalDevice::timestamp_period");
            return properties_.limits.timestampPeriod;
        }

/// Performs the timestamp valid bits operation for `Vulkan` using the supplied arguments.
///
/// @param queue_family_index Zero-based index of the target element or entry.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanPhysicalDevice::timestamp_valid_bits(u32 queue_family_index) const noexcept {
            ZoneScopedN("VulkanPhysicalDevice::timestamp_valid_bits");
            if (queue_family_index >= static_cast<u32>(queue_families_.size()))
                return 0;
            return queue_families_[queue_family_index].timestampValidBits;
        }

/// Returns the current or globally available calibrateable time domains value.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<vector<VkTimeDomainKHR>> VulkanPhysicalDevice::calibrateable_time_domains() const noexcept {
            ZoneScopedN("VulkanPhysicalDevice::calibrateable_time_domains");
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

/// Finds the requested entry in the available state.
///
/// @param surface Surface used or affected by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::findGraphicsQueue(VkSurfaceKHR surface) noexcept {
            ZoneScopedN("VulkanPhysicalDevice::findGraphicsQueue");
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

/// Finds present queue family in the available state.
///
/// @param surface Surface used or affected by the operation.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_present_queue_family(VkSurfaceKHR surface) noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_present_queue_family");
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

/// Performs the queue family supports present operation for `Vulkan` using the supplied arguments.
///
/// @param family_index Zero-based index of the target element or entry.
/// @param surface Surface used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanPhysicalDevice::queue_family_supports_present(u32 family_index, VkSurfaceKHR surface) const noexcept {
            ZoneScopedN("VulkanPhysicalDevice::queue_family_supports_present");
            VkBool32 supported = VK_FALSE;
            return vkGetPhysicalDeviceSurfaceSupportKHR(device_, family_index, surface, &supported) == VK_SUCCESS &&
                   supported == VK_TRUE;
        }

/// Finds compute queue family in the available state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_compute_queue_family() noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_compute_queue_family");
            if (!computeQueueFamIdx.has_value())
                computeQueueFamIdx = find_queue_family_with(VK_QUEUE_COMPUTE_BIT);
            return computeQueueFamIdx;
        }

/// Finds transfer queue family in the available state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_transfer_queue_family() noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_transfer_queue_family");
            if (!transferQueueFamIdx.has_value())
                transferQueueFamIdx = find_queue_family_with(VK_QUEUE_TRANSFER_BIT);
            return transferQueueFamIdx;
        }

/// Finds sparse binding queue family in the available state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_sparse_binding_queue_family() noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_sparse_binding_queue_family");
            if (!sparseBindingQueueFamIdx.has_value())
                sparseBindingQueueFamIdx = find_queue_family_with(VK_QUEUE_SPARSE_BINDING_BIT);
            return sparseBindingQueueFamIdx;
        }

/// Finds protected queue family in the available state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_protected_queue_family() noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_protected_queue_family");
            if (!protectedQueueFamIdx.has_value())
                protectedQueueFamIdx = find_queue_family_with(VK_QUEUE_PROTECTED_BIT);
            return protectedQueueFamIdx;
        }

/// Finds video decode queue family in the available state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_video_decode_queue_family() noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_video_decode_queue_family");
            if (!videoDecodeQueueFamIdx.has_value())
                videoDecodeQueueFamIdx = find_queue_family_with(VK_QUEUE_VIDEO_DECODE_BIT_KHR);
            return videoDecodeQueueFamIdx;
        }

/// Finds video encode queue family in the available state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_video_encode_queue_family() noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_video_encode_queue_family");
            if (!videoEncodeQueueFamIdx.has_value())
                videoEncodeQueueFamIdx = find_queue_family_with(VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
            return videoEncodeQueueFamIdx;
        }

/// Finds optical flow queue family in the available state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_optical_flow_queue_family() noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_optical_flow_queue_family");
            if (!opticalFlowQueueFamIdx.has_value())
                opticalFlowQueueFamIdx = find_queue_family_with(VK_QUEUE_OPTICAL_FLOW_BIT_NV);
            return opticalFlowQueueFamIdx;
        }

/// Enumerates extensions using the supplied arguments and current state.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<VkExtensionProperties>> VulkanPhysicalDevice::enumerate_extensions() const {
            ZoneScopedN("VulkanPhysicalDevice::enumerate_extensions");
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

/// Reports whether extension holds for this `Vulkan`.
///
/// @param name Name used to identify or label the target.
///
/// @return Returns the value produced by the operation.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] bool VulkanPhysicalDevice::supports_extension(string_view name) const {
            ZoneScopedN("VulkanPhysicalDevice::supports_extension");
            auto extensions = enumerate_extensions();
            if (!extensions) return false;
            return std::ranges::any_of(*extensions, [&](const VkExtensionProperties &ext) {
                return name == string_view{ext.extensionName};
            });
        }

/// Performs the surface capabilities operation for `Vulkan` using the supplied arguments.
///
/// @param surface Surface used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkSurfaceCapabilitiesKHR>
        VulkanPhysicalDevice::surface_capabilities(VkSurfaceKHR surface) const noexcept {
            ZoneScopedN("VulkanPhysicalDevice::surface_capabilities");
            VkSurfaceCapabilitiesKHR caps{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_, surface, &caps) != VK_SUCCESS) {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
            }
            return caps;
        }

/// Performs the surface formats operation for `Vulkan` using the supplied arguments.
///
/// @param surface Surface used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<VkSurfaceFormatKHR>>
        VulkanPhysicalDevice::surface_formats(VkSurfaceKHR surface) const {
            ZoneScopedN("VulkanPhysicalDevice::surface_formats");
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

/// Performs the surface present modes operation for `Vulkan` using the supplied arguments.
///
/// @param surface Surface used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<VkPresentModeKHR>>
        VulkanPhysicalDevice::surface_present_modes(VkSurfaceKHR surface) const {
            ZoneScopedN("VulkanPhysicalDevice::surface_present_modes");
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

/// Finds queue family with in the available state.
///
/// @param flags Flags controlling optional behavior.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note Normal inability to produce a value is represented by an empty optional.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<u32> VulkanPhysicalDevice::find_queue_family_with(VkQueueFlags flags) const noexcept {
            ZoneScopedN("VulkanPhysicalDevice::find_queue_family_with");
            auto match = std::ranges::find_if(queue_families_, [flags](const VkQueueFamilyProperties &qf) {
                return (qf.queueFlags & flags) == flags;
            });
            if (match == queue_families_.end())
                return nullopt;
            return static_cast<u32>(match - queue_families_.begin());
        }

} // namespace SFT::Core::Vulkan

namespace SFT::Core::Vulkan {

    /// Returns the current or globally available score value.
    ///
    /// @return Returns the current score value.
    /// @note This function does not throw exceptions.
    f64 VulkanPhysicalDevice::score() const noexcept {
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

} // namespace SFT::Core::Vulkan

