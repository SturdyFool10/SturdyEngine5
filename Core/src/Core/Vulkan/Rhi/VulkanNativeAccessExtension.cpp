#include "VulkanNativeAccessExtension.hpp"

namespace SFT::Core::Vulkan {

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

[[nodiscard]] RHI::ExtensionId VulkanNativeAccessExtension::extension_id() const noexcept { return id(); }

[[nodiscard]] VkInstance VulkanNativeAccessExtension::native_instance() const noexcept { return instance_; }

[[nodiscard]] VkPhysicalDevice VulkanNativeAccessExtension::native_physical_device() const noexcept { return physical_device_; }

[[nodiscard]] VkDevice VulkanNativeAccessExtension::native_device() const noexcept { return device_; }

[[nodiscard]] VkQueue VulkanNativeAccessExtension::native_graphics_queue() const noexcept { return graphics_queue_; }

[[nodiscard]] VkQueue VulkanNativeAccessExtension::native_queue(RHI::QueueLane lane) const noexcept {
            if (queue_lookup_ == nullptr) {
                return lane.queue == RHI::QueueClass::Graphics && lane.index == 0 ? graphics_queue_ : VK_NULL_HANDLE;
            }
            return queue_lookup_(queue_lookup_context_, lane);
        }

[[nodiscard]] u32 VulkanNativeAccessExtension::native_queue_family(RHI::QueueLane lane) const noexcept {
            if (queue_family_lookup_ == nullptr) {
                return ~0u;
            }
            return queue_family_lookup_(queue_lookup_context_, lane);
        }

} // namespace SFT::Core::Vulkan
