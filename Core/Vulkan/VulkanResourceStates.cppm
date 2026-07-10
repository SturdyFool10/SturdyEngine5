module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#pragma endregion

export module Sturdy.Core:VulkanResourceStates;

import Sturdy.Foundation;

export namespace SFT::Core::Vulkan {

    // Renderer-facing image states used by the Vulkan backend when compiling multipass dynamic
    // rendering work. They describe the next use of a subresource; the helpers below translate that
    // use into synchronization2 stage/access/layout fields.
    enum class VulkanImageUse : u8 {
        Undefined,
        Present,
        ColorAttachmentWrite,
        DepthStencilAttachmentWrite,
        DepthAttachmentWrite,
        DepthStencilRead,
        ShaderSampledRead,
        ShaderStorageRead,
        ShaderStorageWrite,
        ShaderStorageReadWrite,
        TransferSrc,
        TransferDst,
        General,
        HostRead,
        HostWrite,
    };

    struct VulkanImageSubresourceRange {
        VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT;
        u32 base_mip = 0;
        u32 mip_count = 1;
        u32 base_layer = 0;
        u32 layer_count = 1;

        [[nodiscard]] VkImageSubresourceRange to_vk() const noexcept {
            return VkImageSubresourceRange{
                .aspectMask = aspects,
                .baseMipLevel = base_mip,
                .levelCount = mip_count,
                .baseArrayLayer = base_layer,
                .layerCount = layer_count,
            };
        }
    };

    [[nodiscard]] constexpr VulkanImageSubresourceRange full_image_range(
        VkImageAspectFlags aspects,
        u32 mip_count = VK_REMAINING_MIP_LEVELS,
        u32 layer_count = VK_REMAINING_ARRAY_LAYERS) noexcept {
        return VulkanImageSubresourceRange{
            .aspects = aspects,
            .mip_count = mip_count,
            .layer_count = layer_count,
        };
    }

    struct VulkanImageState {
        VkPipelineStageFlags2 stages = VK_PIPELINE_STAGE_2_NONE;
        VkAccessFlags2 access = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    [[nodiscard]] constexpr bool is_depth_stencil_aspect(VkImageAspectFlags aspects) noexcept {
        return (aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
    }

    [[nodiscard]] constexpr VulkanImageState image_state_for_use(VulkanImageUse use,
                                                                 VkImageAspectFlags aspects = VK_IMAGE_ASPECT_COLOR_BIT) noexcept {
        switch (use) {
            case VulkanImageUse::Undefined:
                return {};
            case VulkanImageUse::Present:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_NONE,
                    .access = 0,
                    .layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                };
            case VulkanImageUse::ColorAttachmentWrite:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    .access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                };
            case VulkanImageUse::DepthStencilAttachmentWrite:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                };
            case VulkanImageUse::DepthAttachmentWrite:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    .layout = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0
                                  ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                };
            case VulkanImageUse::DepthStencilRead:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    .access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    .layout = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0
                                  ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                  : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                };
            case VulkanImageUse::ShaderSampledRead:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                };
            case VulkanImageUse::ShaderStorageRead:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_GENERAL,
                };
            case VulkanImageUse::ShaderStorageWrite:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .access = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_GENERAL,
                };
            case VulkanImageUse::ShaderStorageReadWrite:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR,
                    .access = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_GENERAL,
                };
            case VulkanImageUse::TransferSrc:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT,
                    .access = VK_ACCESS_2_TRANSFER_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                };
            case VulkanImageUse::TransferDst:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
                               VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    .access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                };
            case VulkanImageUse::General:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    .access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_GENERAL,
                };
            case VulkanImageUse::HostRead:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_HOST_BIT,
                    .access = VK_ACCESS_2_HOST_READ_BIT,
                    .layout = VK_IMAGE_LAYOUT_GENERAL,
                };
            case VulkanImageUse::HostWrite:
                return VulkanImageState{
                    .stages = VK_PIPELINE_STAGE_2_HOST_BIT,
                    .access = VK_ACCESS_2_HOST_WRITE_BIT,
                    .layout = VK_IMAGE_LAYOUT_GENERAL,
                };
        }
        return {};
    }

    struct VulkanImageTransition {
        VkImage image = VK_NULL_HANDLE;
        VulkanImageState before{};
        VulkanImageState after{};
        VulkanImageSubresourceRange range{};
        u32 src_queue_family = VK_QUEUE_FAMILY_IGNORED;
        u32 dst_queue_family = VK_QUEUE_FAMILY_IGNORED;
    };

    [[nodiscard]] constexpr VkImageMemoryBarrier2 image_barrier(const VulkanImageTransition &transition) noexcept {
        return VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = transition.before.stages,
            .srcAccessMask = transition.before.access,
            .dstStageMask = transition.after.stages,
            .dstAccessMask = transition.after.access,
            .oldLayout = transition.before.layout,
            .newLayout = transition.after.layout,
            .srcQueueFamilyIndex = transition.src_queue_family,
            .dstQueueFamilyIndex = transition.dst_queue_family,
            .image = transition.image,
            .subresourceRange = transition.range.to_vk(),
        };
    }

    [[nodiscard]] constexpr VkImageMemoryBarrier2 image_barrier(VkImage image,
                                                                VulkanImageUse before,
                                                                VulkanImageUse after,
                                                                VulkanImageSubresourceRange range) noexcept {
        return image_barrier(VulkanImageTransition{
            .image = image,
            .before = image_state_for_use(before, range.aspects),
            .after = image_state_for_use(after, range.aspects),
            .range = range,
        });
    }

    [[nodiscard]] constexpr VkDependencyInfo dependency_for_image_barrier(const VkImageMemoryBarrier2 &barrier) noexcept {
        return VkDependencyInfo{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = 0,
            .pMemoryBarriers = nullptr,
            .bufferMemoryBarrierCount = 0,
            .pBufferMemoryBarriers = nullptr,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier,
        };
    }

} // namespace SFT::Core::Vulkan
