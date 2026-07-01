module;
#include "volk.h"
#include <span>
#include <vector>

export module Sturdy.Core:VulkanPipeline;

import :RendererError;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using std::span;
using std::vector;

export namespace SFT::Core::Vulkan {

    // ─── VulkanPipelineLayout ────────────────────────────────────────────────────

    class VulkanPipelineLayout {
      public:
        VulkanPipelineLayout() = default;
        ~VulkanPipelineLayout() { destroy(); }

        VulkanPipelineLayout(const VulkanPipelineLayout &) = delete;
        VulkanPipelineLayout &operator=(const VulkanPipelineLayout &) = delete;

        VulkanPipelineLayout(VulkanPipelineLayout &&o) noexcept
            : device_(o.device_), layout_(o.layout_) {
            o.device_ = VK_NULL_HANDLE;
            o.layout_ = VK_NULL_HANDLE;
        }
        VulkanPipelineLayout &operator=(VulkanPipelineLayout &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                layout_ = o.layout_;
                o.device_ = VK_NULL_HANDLE;
                o.layout_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create(
            VkDevice device,
            const VkPipelineLayoutCreateInfo &info) noexcept {
            VkPipelineLayout layout = VK_NULL_HANDLE;
            if (vkCreatePipelineLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreatePipelineLayout failed.");
            VulkanPipelineLayout out;
            out.device_ = device;
            out.layout_ = layout;
            return out;
        }

        // Convenience: empty layout (no push constants, no descriptor sets).
        [[nodiscard]] static RendererExpected<VulkanPipelineLayout> create_empty(VkDevice device) noexcept {
            VkPipelineLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .setLayoutCount = 0,
                .pSetLayouts = nullptr,
                .pushConstantRangeCount = 0,
                .pPushConstantRanges = nullptr,
            };
            return create(device, info);
        }

        [[nodiscard]] VkPipelineLayout vk_handle() const noexcept { return layout_; }
        [[nodiscard]] bool is_valid() const noexcept { return layout_ != VK_NULL_HANDLE; }

        void destroy() noexcept {
            if (layout_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineLayout(device_, layout_, nullptr);
            layout_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineLayout layout_ = VK_NULL_HANDLE;
    };

    // ─── VulkanPipeline ──────────────────────────────────────────────────────────

    class VulkanPipeline {
      public:
        VulkanPipeline() = default;
        ~VulkanPipeline() { destroy(); }

        VulkanPipeline(const VulkanPipeline &) = delete;
        VulkanPipeline &operator=(const VulkanPipeline &) = delete;

        VulkanPipeline(VulkanPipeline &&o) noexcept
            : device_(o.device_), pipeline_(o.pipeline_), bind_point_(o.bind_point_) {
            o.device_ = VK_NULL_HANDLE;
            o.pipeline_ = VK_NULL_HANDLE;
        }
        VulkanPipeline &operator=(VulkanPipeline &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                pipeline_ = o.pipeline_;
                bind_point_ = o.bind_point_;
                o.device_ = VK_NULL_HANDLE;
                o.pipeline_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // For pipelines used with traditional render passes.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics(
            VkDevice device,
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept {
            if (device == VK_NULL_HANDLE)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateGraphicsPipelines called with a null VkDevice.");
            if (vkCreateGraphicsPipelines == nullptr)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateGraphicsPipelines is not loaded. Call volkLoadDevice after device creation.");

            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateGraphicsPipelines failed.");
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
            return out;
        }

        // For pipelines used with vkCmdBeginRendering (Vulkan 1.3+ dynamic rendering).
        // Set info.renderPass = VK_NULL_HANDLE and chain a VkPipelineRenderingCreateInfo
        // (from PipelineRenderingInfo::to_vk()) into info.pNext describing the attachment formats.
        [[nodiscard]] static RendererExpected<VulkanPipeline> create_graphics_dynamic(
            VkDevice device,
            VkPipelineCache cache,
            VkGraphicsPipelineCreateInfo info // taken by value so we can assert renderPass is null
            ) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed,
                                      "vkCreateGraphicsPipelines (dynamic rendering) failed.");
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
            return out;
        }

        [[nodiscard]] static RendererExpected<VulkanPipeline> create_compute(
            VkDevice device,
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateComputePipelines failed.");
            VulkanPipeline out;
            out.device_ = device;
            out.pipeline_ = pipeline;
            out.bind_point_ = VK_PIPELINE_BIND_POINT_COMPUTE;
            return out;
        }

        [[nodiscard]] VkPipeline vk_handle() const noexcept { return pipeline_; }
        [[nodiscard]] bool is_valid() const noexcept { return pipeline_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkPipelineBindPoint bind_point() const noexcept { return bind_point_; }
        [[nodiscard]] bool is_graphics() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_GRAPHICS; }
        [[nodiscard]] bool is_compute() const noexcept { return bind_point_ == VK_PIPELINE_BIND_POINT_COMPUTE; }

        void destroy() noexcept {
            if (pipeline_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipeline pipeline_ = VK_NULL_HANDLE;
        VkPipelineBindPoint bind_point_ = VK_PIPELINE_BIND_POINT_GRAPHICS;
    };

    // ─── VulkanPipelineCache ─────────────────────────────────────────────────────

    class VulkanPipelineCache {
      public:
        VulkanPipelineCache() = default;
        ~VulkanPipelineCache() { destroy(); }

        VulkanPipelineCache(const VulkanPipelineCache &) = delete;
        VulkanPipelineCache &operator=(const VulkanPipelineCache &) = delete;

        VulkanPipelineCache(VulkanPipelineCache &&o) noexcept
            : device_(o.device_), cache_(o.cache_) {
            o.device_ = VK_NULL_HANDLE;
            o.cache_ = VK_NULL_HANDLE;
        }
        VulkanPipelineCache &operator=(VulkanPipelineCache &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                cache_ = o.cache_;
                o.device_ = VK_NULL_HANDLE;
                o.cache_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // Pass previously saved cache data to seed the cache; pass an empty span to start fresh.
        [[nodiscard]] static RendererExpected<VulkanPipelineCache> create(
            VkDevice device,
            span<const u8> initial_data = {}) noexcept {
            VkPipelineCacheCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .initialDataSize = initial_data.size_bytes(),
                .pInitialData = initial_data.empty() ? nullptr : initial_data.data(),
            };
            VkPipelineCache cache = VK_NULL_HANDLE;
            if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreatePipelineCache failed.");
            VulkanPipelineCache out;
            out.device_ = device;
            out.cache_ = cache;
            return out;
        }

        [[nodiscard]] VkPipelineCache vk_handle() const noexcept { return cache_; }
        [[nodiscard]] bool is_valid() const noexcept { return cache_ != VK_NULL_HANDLE; }

        // Serializes the cache to a byte blob suitable for saving to disk and re-seeding next run.
        [[nodiscard]] RendererExpected<vector<u8>> serialize() const {
            usize size = 0;
            if (vkGetPipelineCacheData(device_, cache_, &size, nullptr) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetPipelineCacheData (size) failed.");
            vector<u8> data(size);
            if (vkGetPipelineCacheData(device_, cache_, &size, data.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetPipelineCacheData (read) failed.");
            return data;
        }

        void destroy() noexcept {
            if (cache_ == VK_NULL_HANDLE)
                return;
            vkDestroyPipelineCache(device_, cache_, nullptr);
            cache_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache cache_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
