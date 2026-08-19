#include <Core/Vulkan/Rhi/VulkanNativeAccessExtension.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Performs the vulkan native access extension operation for `Vulkan` using the supplied arguments.
///
/// @param instance Instance used or affected by the operation.
/// @param physical_device Device used or affected by the operation.
/// @param device Device used or affected by the operation.
/// @param graphics_queue Queue used or affected by the operation.
/// @param queue_lookup_context Context that supplies state required by the operation.
/// @param queue_lookup Queue used or affected by the operation.
/// @param queue_family_lookup Queue used or affected by the operation.
///
/// @note This function does not throw exceptions.
VulkanNativeAccessExtension::VulkanNativeAccessExtension(VkInstance instance,
                                    VkPhysicalDevice physical_device,
                                    VkDevice device,
                                    VkQueue graphics_queue,
                                    void *queue_lookup_context,
                                    VulkanNativeAccessExtension::NativeQueueLookup queue_lookup,
                                    VulkanNativeAccessExtension::NativeQueueFamilyLookup queue_family_lookup) noexcept
            : instance_(instance), physical_device_(physical_device), device_(device),
              graphics_queue_(graphics_queue), queue_lookup_context_(queue_lookup_context),
              queue_lookup_(queue_lookup), queue_family_lookup_(queue_family_lookup) {}

/// Returns the current or globally available extension ID value.
///
/// @return Returns the current extension ID value.
/// @note This function does not throw exceptions.
[[nodiscard]] RHI::ExtensionId VulkanNativeAccessExtension::extension_id() const noexcept { return id(); }

/// Returns the current or globally available native instance value.
///
/// @return Returns the current native instance value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkInstance VulkanNativeAccessExtension::native_instance() const noexcept { return instance_; }

/// Returns the current or globally available native physical device value.
///
/// @return Returns the current native physical device value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPhysicalDevice VulkanNativeAccessExtension::native_physical_device() const noexcept { return physical_device_; }

/// Returns the current or globally available native device value.
///
/// @return Returns the current native device value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDevice VulkanNativeAccessExtension::native_device() const noexcept { return device_; }

/// Returns the current or globally available native graphics queue value.
///
/// @return Returns the current native graphics queue value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkQueue VulkanNativeAccessExtension::native_graphics_queue() const noexcept { return graphics_queue_; }

/// Performs the native queue operation for `Vulkan` using the supplied arguments.
///
/// @param lane `lane` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkQueue VulkanNativeAccessExtension::native_queue(RHI::QueueLane lane) const noexcept {
            ZoneScopedN("VulkanNativeAccessExtension::VulkanNativeAccessExtension");
            if (queue_lookup_ == nullptr) {
                return lane.queue == RHI::QueueClass::Graphics && lane.index == 0 ? graphics_queue_ : VK_NULL_HANDLE;
            }
            return queue_lookup_(queue_lookup_context_, lane);
        }

/// Performs the native queue family operation for `Vulkan` using the supplied arguments.
///
/// @param lane `lane` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] u32 VulkanNativeAccessExtension::native_queue_family(RHI::QueueLane lane) const noexcept {
            ZoneScopedN("VulkanNativeAccessExtension::native_queue_family");
            if (queue_family_lookup_ == nullptr) {
                return ~0u;
            }
            return queue_family_lookup_(queue_lookup_context_, lane);
        }

} // namespace SFT::Core::Vulkan
