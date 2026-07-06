// VulkanBackend buffer bring-up: uploads the demo hexagon's vertices and indices into
// device-local buffers via temporary staging buffers and a single one-shot command buffer copy.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <format>
#include <span>
#pragma endregion

module Sturdy.Core;

import :VulkanAllocator;
import :VulkanBackend;
import :VulkanBuffer;
import :VulkanCommandBuffer;
import :VulkanCommandPool;
import :VulkanSync;
import :VulkanVertex;
import :RendererError;
import :Renderer;
import Sturdy.Foundation;

using std::format;
using std::span;

namespace SFT::Core::Vulkan {

    namespace {

        // Host-visible staging buffer, seeded with `data`/`size`, plus the device-local buffer
        // (created with `usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT`) it will be copied into. The
        // staging buffer must stay alive until the copy this seeds has actually been submitted and
        // waited on — see the single one-shot submission in createGeometryBuffers() below.
        struct StagedBuffer {
            VulkanBuffer staging;
            VulkanBuffer device_local;
        };

        [[nodiscard]] RendererExpected<StagedBuffer> stage_buffer(
            const VulkanAllocator &allocator,
            VkDevice device,
            const void *data,
            VkDeviceSize size,
            VkBufferUsageFlags device_local_usage,
            const char *what) noexcept {
            const VkBufferCreateInfo staging_info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };
            const VmaAllocationCreateInfo staging_alloc_info{
                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
            };
            auto staging_result = allocator.create_buffer(device, staging_info, staging_alloc_info);
            if (!staging_result.has_value()) [[unlikely]] {
                return renderer_error(staging_result.error().code,
                                      format("Failed to create {} staging buffer: {}", what, staging_result.error().message));
            }
            VulkanBuffer staging = std::move(*staging_result);

            if (auto upload_result = staging.upload(data, size); !upload_result.has_value()) [[unlikely]] {
                return renderer_error(upload_result.error().code,
                                      format("Failed to upload {} data to staging buffer: {}", what, upload_result.error().message));
            }

            const VkBufferCreateInfo device_local_info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = device_local_usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };
            const VmaAllocationCreateInfo device_local_alloc_info{
                .usage = VMA_MEMORY_USAGE_AUTO,
            };
            auto device_local_result = allocator.create_buffer(device, device_local_info, device_local_alloc_info);
            if (!device_local_result.has_value()) [[unlikely]] {
                return renderer_error(device_local_result.error().code,
                                      format("Failed to create device-local {} buffer: {}", what, device_local_result.error().message));
            }

            return StagedBuffer{std::move(staging), std::move(*device_local_result)};
        }

    } // namespace

    RendererResult VulkanBackend::createGeometryBuffers(const RendererCreateInfo &init) {
        (void)init;

        constexpr auto vertices = hexagon_vertices();
        constexpr auto indices = hexagon_indices();
        constexpr VkDeviceSize vertex_bytes = sizeof(vertices);
        constexpr VkDeviceSize index_bytes = sizeof(indices);

        auto staged_vertices = stage_buffer(this->vmaAllocator, this->logicalDevice.vk_handle(),
                                            vertices.data(), vertex_bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, "vertex");
        if (!staged_vertices.has_value()) [[unlikely]] {
            return renderer_error(staged_vertices.error().code, staged_vertices.error().message);
        }

        auto staged_indices = stage_buffer(this->vmaAllocator, this->logicalDevice.vk_handle(),
                                           indices.data(), index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "index");
        if (!staged_indices.has_value()) [[unlikely]] {
            return renderer_error(staged_indices.error().code, staged_indices.error().message);
        }

        // One-shot: a transient pool/buffer/fence that only exist for the duration of these copies —
        // both the vertex and index upload are recorded into the same command buffer and submitted
        // together, rather than one one-shot submission per buffer.
        auto pool_result = VulkanCommandPool::create(
            this->logicalDevice.vk_handle(),
            this->gfxQueue.family_index(),
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
        if (!pool_result.has_value()) [[unlikely]] {
            return renderer_error(pool_result.error().code,
                                  format("Failed to create transient command pool for geometry upload: {}", pool_result.error().message));
        }
        VulkanCommandPool transient_pool = std::move(*pool_result);

        auto cmd_result = VulkanCommandBuffer::allocate(this->logicalDevice.vk_handle(), transient_pool.vk_handle());
        if (!cmd_result.has_value()) [[unlikely]] {
            return renderer_error(cmd_result.error().code,
                                  format("Failed to allocate one-shot command buffer for geometry upload: {}", cmd_result.error().message));
        }
        VulkanCommandBuffer cmd = std::move(*cmd_result);

        if (auto begin_result = cmd.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT); !begin_result.has_value()) [[unlikely]] {
            return renderer_error(begin_result.error().code,
                                  format("Failed to begin one-shot command buffer for geometry upload: {}", begin_result.error().message));
        }
        cmd.copy_buffer(staged_vertices->staging.vk_handle(), staged_vertices->device_local.vk_handle(), vertex_bytes);
        cmd.copy_buffer(staged_indices->staging.vk_handle(), staged_indices->device_local.vk_handle(), index_bytes);
        if (auto end_result = cmd.end(); !end_result.has_value()) [[unlikely]] {
            return renderer_error(end_result.error().code,
                                  format("Failed to end one-shot command buffer for geometry upload: {}", end_result.error().message));
        }

        auto fence_result = VulkanFence::create(this->logicalDevice.vk_handle());
        if (!fence_result.has_value()) [[unlikely]] {
            return renderer_error(fence_result.error().code,
                                  format("Failed to create fence for geometry upload: {}", fence_result.error().message));
        }
        VulkanFence fence = std::move(*fence_result);

        if (auto submit_result = this->gfxQueue.submit(
                cmd.submit_info(),
                span<const VkSemaphoreSubmitInfo>{},
                span<const VkSemaphoreSubmitInfo>{},
                fence.vk_handle());
            !submit_result.has_value()) [[unlikely]] {
            return renderer_error(submit_result.error().code,
                                  format("Failed to submit geometry upload copy: {}", submit_result.error().message));
        }

        if (auto wait_result = fence.wait(); !wait_result.has_value()) [[unlikely]] {
            return renderer_error(wait_result.error().code,
                                  format("Failed to wait for geometry upload copy to finish: {}", wait_result.error().message));
        }

        // fence/cmd/transient_pool/both staging buffers all fall out of scope here (RAII) — the
        // copies have already completed (we just waited on their fence), so it's safe to release
        // them now.
        this->vertexBuffer = std::move(staged_vertices->device_local);
        this->indexBuffer = std::move(staged_indices->device_local);
        this->indexCount = static_cast<u32>(indices.size());
        this->indexType = VK_INDEX_TYPE_UINT16;

        Foundation::log_info("Vulkan geometry buffers created: {} vertices, {} indices ({} + {} bytes).",
                             vertices.size(), this->indexCount, vertex_bytes, index_bytes);
        return {};
    }

} // namespace SFT::Core::Vulkan
