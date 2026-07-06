// VulkanBackend per-frame work: the render_frame() record/submit/present loop and the
// per-surface frame resources (timeline semaphore, per-frame command pools/buffers).
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

#include <format>
#include <limits>
#include <span>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :VulkanBackend;
import :VulkanCommandBuffer;
import :VulkanCommandPool;
import :VulkanDevice;
import :VulkanPipeline;
import :VulkanQueue;
import :VulkanSurface;
import :VulkanSwapchain;
import :VulkanSync;
import :RendererError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;

using std::format;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    RendererResult VulkanBackend::render_frame(RenderSurfaceHandle surface, const FrameInput &frame) {
        (void)frame;

        VulkanSurface *s = surface_slot(surface);
        if (!s || !s->has_frame_resources()) [[unlikely]] {
            // Unknown/removed window, or a surface that never had its frame resources built.
            return {};
        }
        s->refresh_extent();
        if (s->swapchain_dirty() and not s->extent().is_zero()) [[unlikely]] {
            wait_idle();
            // Don't tear the old swapchain down first: createSwapchain feeds it to the driver as
            // oldSwapchain and only releases it once the replacement is built (see set_swapchain),
            // which lets the driver reuse the retiring backing instead of committing a fresh set.
            if (auto result = createSwapchain(create_info_, *s); !result.has_value()) [[unlikely]] {
                return renderer_error(result.error().code,
                                      format("Failed to recreate swapchain after resize: {}", result.error().message));
            }
            s->clear_dirty();
        }

        // Frame pacing is per-surface, so each window throttles and records independently.
        const auto ticket = s->begin_frame();
        auto _ = s->frame_timeline().wait(ticket.wait_value, std::numeric_limits<u64>::max());

        FrameResources& res = *ticket.resources;
        auto _poolReset = res.commandPool.reset(0);

        auto& AcquireSem = res.imageAcquiredSemaphore;

        auto acquire_result = s->swapchain().acquire_next_image(AcquireSem.vk_handle());
        if (!acquire_result.has_value()) [[unlikely]] {
            s->mark_dirty();
            return {}; // we cannot render, return early
        }
        const u32 imageIndex = *acquire_result;

        //begin the command buffer record
        auto _buffBeginRes = res.commandBuffer.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

        //transition the color and depth images, then begin dynamic rendering into them
        res.commandBuffer.pipeline_barrier2(s->swapchain().undefined_to_attachment_barriers(imageIndex));

        const VkExtent2D swapExtent = s->swapchain().extent();
        const VkRect2D renderArea{
            .offset = {.x = 0, .y = 0},
            .extent = swapExtent,
        };
        const auto attachments = s->swapchain().rendering_attachments(imageIndex);

        res.commandBuffer.begin_rendering(attachments.rendering_info(renderArea));
        {
            res.commandBuffer.set_viewport(VkViewport{
                .x = 0, .y = 0,
                .width  = static_cast<float>(swapExtent.width),
                .height = static_cast<float>(swapExtent.height),
            });
            res.commandBuffer.set_scissor(renderArea);

            //the all important drawing
            res.commandBuffer.bind_pipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, this->graphicsPipeline.vk_handle());
            res.commandBuffer.draw(3);
        }
        res.commandBuffer.end_rendering();

        //transition the image from color attachment to presentation so it can be presented
        res.commandBuffer.pipeline_barrier2(s->swapchain().attachment_to_present_barrier(imageIndex));

        if (auto endRes = res.commandBuffer.end(); !endRes.has_value()) [[unlikely]] {
            return renderer_error(endRes.error().code,
                                  format("Failed to end command buffer: {}", endRes.error().message));
        }

        // ensure swapchain image is actually available to start color output
        const VkSemaphoreSubmitInfo imageAcquireWaitInfo =
            AcquireSem.submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

        // signal that the image can be presented
        const vector<VkSemaphoreSubmitInfo> semaphoreSignals{
            // render work completion signal
            s->swapchain().render_finished_semaphores()[imageIndex].submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT),
            // entire frame is completed (timeline)
            s->frame_timeline().submit_info(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, ticket.signal_value),
        };

        if (auto submitRes = this->gfxQueue.submit(
                res.commandBuffer.submit_info(),
                span{&imageAcquireWaitInfo, 1},
                semaphoreSignals);
            !submitRes.has_value()) [[unlikely]] {
            return renderer_error(submitRes.error().code,
                                  format("Failed to submit frame command buffer: {}", submitRes.error().message));
        }

        const auto presentRequest = s->swapchain().present_request(imageIndex);
        auto presentRes = this->gfxQueue.present(presentRequest.present_info());
        if (!presentRes.has_value()) [[unlikely]] {
            return renderer_error(presentRes.error().code,
                                  format("Failed to present frame: {}", presentRes.error().message));
        }
        // VK_SUBOPTIMAL_KHR (*presentRes == true) is tolerated silently during live resize —
        // rebuilding the swapchain every frame (and calling wait_idle each time) causes severe
        // jitter. The swapchain is rebuilt at a sensible cadence via on_surface_resize_needed()
        // (fired from the event loop after the drag ends) or when acquire returns OUT_OF_DATE.
        (void)presentRes;

        return {};
    }

    RendererResult VulkanBackend::createSurfaceFrameResources(VulkanSurface &surface) {
        const u32 frame_count = surface.frames_in_flight();
        // Seed the timeline at frame_count so the first frame_count frames wait on already-signalled
        // values and never block; each frame's submission then signals the next value up (see
        // VulkanSurface::begin_frame / set_frame_resources).
        const u64 initial_timeline_value = frame_count;

        auto timeline_result = VulkanSemaphore::create_timeline(this->logicalDevice.vk_handle(), initial_timeline_value);
        if (!timeline_result.has_value()) [[unlikely]] {
            return renderer_error(timeline_result.error().code,
                                  format("Failed to create frame timeline semaphore: {}", timeline_result.error().message));
        }

        // Partially built frames clean themselves up: FrameResources' members are RAII wrappers, so
        // an early return destroys the vector and releases whatever was created so far.
        vector<FrameResources> frames;
        frames.reserve(frame_count);
        for (u32 frame_index = 0; frame_index < frame_count; ++frame_index) {
            FrameResources resources{};

            auto image_acquired_result = VulkanSemaphore::create_binary(this->logicalDevice.vk_handle());
            if (!image_acquired_result.has_value()) [[unlikely]] {
                return renderer_error(image_acquired_result.error().code,
                                      format("Failed to create image-acquired semaphore for frame {}: {}",
                                             frame_index,
                                             image_acquired_result.error().message));
            }
            resources.imageAcquiredSemaphore = std::move(*image_acquired_result);

            // Each frame gets its own pool for faster resets.
            auto command_pool_result = VulkanCommandPool::create(
                this->logicalDevice.vk_handle(),
                this->gfxQueue.family_index());
            if (!command_pool_result.has_value()) [[unlikely]] {
                return renderer_error(command_pool_result.error().code,
                                      format("Failed to create graphics command pool for frame {}: {}",
                                             frame_index,
                                             command_pool_result.error().message));
            }
            resources.commandPool = std::move(*command_pool_result);

            auto command_buffer_result = VulkanCommandBuffer::allocate(
                this->logicalDevice.vk_handle(),
                resources.commandPool.vk_handle());
            if (!command_buffer_result.has_value()) [[unlikely]] {
                return renderer_error(command_buffer_result.error().code,
                                      format("Failed to create graphics command buffer for frame {}: {}",
                                             frame_index,
                                             command_buffer_result.error().message));
            }
            resources.commandBuffer = std::move(*command_buffer_result);

            frames.push_back(std::move(resources));
        }

        surface.set_frame_resources(std::move(frames), std::move(*timeline_result), initial_timeline_value + 1);

        Foundation::log_info("Vulkan surface frame resources created: frames={} timeline_initial={}",
                             frame_count,
                             initial_timeline_value);
        return {};
    }

} // namespace SFT::Core::Vulkan
