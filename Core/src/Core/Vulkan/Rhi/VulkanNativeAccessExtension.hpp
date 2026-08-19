#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#pragma endregion

#include <RHI/RHI.hpp>

namespace SFT::Core::Vulkan {


    class VulkanNativeAccessExtension final : public RHI::RhiDeviceExtension {
      public:
        /// Returns the current or globally available ID value.
        ///
        /// @return Returns the current ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static constexpr RHI::ExtensionId id() noexcept {
            return RHI::ExtensionId{"sturdy", "vulkan-native-access", 1};
        }

        using NativeQueueLookup = VkQueue (*)(void *context, RHI::QueueLane lane) noexcept;
        using NativeQueueFamilyLookup = u32 (*)(void *context, RHI::QueueLane lane) noexcept;

        /// Constructs a `VulkanNativeAccessExtension` from the supplied initialization values.
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
        VulkanNativeAccessExtension(VkInstance instance,
                                    VkPhysicalDevice physical_device,
                                    VkDevice device,
                                    VkQueue graphics_queue,
                                    void *queue_lookup_context = nullptr,
                                    NativeQueueLookup queue_lookup = nullptr,
                                    NativeQueueFamilyLookup queue_family_lookup = nullptr) noexcept;

        /// Returns the current or globally available extension ID value.
        ///
        /// @return Returns the current extension ID value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::ExtensionId extension_id() const noexcept override;

        /// Returns the current or globally available native instance value.
        ///
        /// @return Returns the current native instance value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkInstance native_instance() const noexcept;
        /// Returns the current or globally available native physical device value.
        ///
        /// @return Returns the current native physical device value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkPhysicalDevice native_physical_device() const noexcept;
        /// Returns the current or globally available native device value.
        ///
        /// @return Returns the current native device value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDevice native_device() const noexcept;
        /// Returns the current or globally available native graphics queue value.
        ///
        /// @return Returns the current native graphics queue value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkQueue native_graphics_queue() const noexcept;


        /// Performs the native queue operation for `VulkanNativeAccessExtension` using the supplied arguments.
        ///
        /// @param lane `lane` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkQueue native_queue(RHI::QueueLane lane) const noexcept;
        /// Performs the native queue family operation for `VulkanNativeAccessExtension` using the supplied arguments.
        ///
        /// @param lane `lane` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 native_queue_family(RHI::QueueLane lane) const noexcept;


        /// Performs the native command buffer operation for `VulkanNativeAccessExtension` using the supplied arguments.
        ///
        /// @param encoder `encoder` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCommandBuffer native_command_buffer(const RHI::CommandEncoder &encoder) const noexcept;

      private:
        VkInstance instance_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
        VkDevice device_ = VK_NULL_HANDLE;
        VkQueue graphics_queue_ = VK_NULL_HANDLE;
        void *queue_lookup_context_ = nullptr;
        NativeQueueLookup queue_lookup_ = nullptr;
        NativeQueueFamilyLookup queue_family_lookup_ = nullptr;
    };

} // namespace SFT::Core::Vulkan
