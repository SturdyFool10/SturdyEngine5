#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#pragma endregion

#include <RHI/RHI.hpp>

namespace SFT::Core::Vulkan {

    // The RHI's `:Extensions` graduation pattern (RHI/Extensions.cppm), instantiated for Vulkan: the
    // one escape hatch a caller needs when Sturdy.RHI hasn't modeled a capability yet — vendor upscaler
    // SDK init (FSR2/DLSS/XeSS), vendor-specific queries (NVAPI/AMD AGS), RenderDoc/Nsight
    // instrumentation, or raw vkCmd* calls interleaved into an RHI-recorded command buffer.
    //
    // Opt-in only: `RhiDevice::extension_interface()` only returns this when the app requested it via
    // `RendererFeatureRequest::enable_native_access_extension` (see Core/Renderer.cppm) at device
    // creation — never enabled implicitly. Once obtained, everything the caller does with these native
    // handles is outside RHI's tracking guarantees (no automatic barrier/lifetime tracking), the same
    // way the RHI's explicit `:Barrier` model already puts synchronization correctness on the caller.
    class VulkanNativeAccessExtension final : public RHI::RhiDeviceExtension {
      public:
        [[nodiscard]] static constexpr RHI::ExtensionId id() noexcept {
            return RHI::ExtensionId{"sturdy", "vulkan-native-access", 1};
        }

        using NativeQueueLookup = VkQueue (*)(void *context, RHI::QueueLane lane) noexcept;
        using NativeQueueFamilyLookup = u32 (*)(void *context, RHI::QueueLane lane) noexcept;

        VulkanNativeAccessExtension(VkInstance instance,
                                    VkPhysicalDevice physical_device,
                                    VkDevice device,
                                    VkQueue graphics_queue,
                                    void *queue_lookup_context = nullptr,
                                    NativeQueueLookup queue_lookup = nullptr,
                                    NativeQueueFamilyLookup queue_family_lookup = nullptr) noexcept;

        [[nodiscard]] RHI::ExtensionId extension_id() const noexcept override;

        [[nodiscard]] VkInstance native_instance() const noexcept;
        [[nodiscard]] VkPhysicalDevice native_physical_device() const noexcept;
        [[nodiscard]] VkDevice native_device() const noexcept;
        [[nodiscard]] VkQueue native_graphics_queue() const noexcept;
        // Returns the native queue backing an advertised RHI queue lane, or VK_NULL_HANDLE if the lane
        // is not exposed. Use RhiDevice::queue_infos() first to discover valid QueueClass/index pairs.
        [[nodiscard]] VkQueue native_queue(RHI::QueueLane lane) const noexcept;
        [[nodiscard]] u32 native_queue_family(RHI::QueueLane lane) const noexcept;

        // Returns the live VkCommandBuffer backing `encoder` while it is still open (between its
        // creation via RhiDevice::create_command_encoder() and its finish()), or VK_NULL_HANDLE if
        // `encoder` isn't the Vulkan bridge's own encoder type. Defined in VulkanRhiBridgeCommands.cpp,
        // the only translation unit where the concrete encoder type is visible.
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
