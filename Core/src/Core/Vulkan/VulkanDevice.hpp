#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <vector>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>

using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::optional;
using std::span;
using std::vector;

namespace SFT::Core::Vulkan {

    // Owns a VkDevice and the queue handles retrieved at creation time.
    // All Vulkan device-level operations are routed through this class.
    // Move-only; destroyed via destroy() or the destructor (whichever comes first).
    class VulkanDevice {
      public:
        // Descriptor passed to create() — caller fills in which queue families to
        // request and which device extensions / feature chain to enable.
        struct DeviceCreateDesc {
            optional<u32> graphics_queue_family{};
            optional<u32> present_queue_family{};
            optional<u32> compute_queue_family{};
            optional<u32> transfer_queue_family{};
            optional<u32> sparse_queue_family{};
            optional<u32> video_decode_queue_family{};
            optional<u32> video_encode_queue_family{};
            u32 graphics_queue_count = 1;
            u32 compute_queue_count = 1;
            u32 transfer_queue_count = 1;
            u32 sparse_queue_count = 1;
            u32 video_decode_queue_count = 1;
            u32 video_encode_queue_count = 1;
            span<const char *> extensions{};
            // Optional pNext chain for VkDeviceCreateInfo — use to pass
            // VkPhysicalDeviceFeatures2 and any feature structs.
            const void *features_pnext = nullptr;
        };

        VulkanDevice() = default;

        ~VulkanDevice();

        VulkanDevice(const VulkanDevice &) = delete;
        VulkanDevice &operator=(const VulkanDevice &) = delete;

        VulkanDevice(VulkanDevice &&o) noexcept;

        VulkanDevice &operator=(VulkanDevice &&o) noexcept;

        // Creates a VkDevice from a pre-selected physical device + descriptor.
        // Deduplicates queue families automatically so the same index is not
        // requested twice (Vulkan spec requires unique family indices in pQueueCreateInfos).
        [[nodiscard]] static RendererExpected<VulkanDevice> create(
            VkPhysicalDevice physical,
            const DeviceCreateDesc &desc) noexcept;

        // -------------------------------------------------------------------------
        // Handle / validity
        // -------------------------------------------------------------------------

        [[nodiscard]] VkDevice vk_handle() const noexcept;
        [[nodiscard]] VkPhysicalDevice physical_vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;

        [[nodiscard]] optional<VulkanQueue> &graphics_queue() noexcept;
        [[nodiscard]] optional<VulkanQueue> &present_queue() noexcept;
        [[nodiscard]] optional<VulkanQueue> &compute_queue() noexcept;
        [[nodiscard]] optional<VulkanQueue> &transfer_queue() noexcept;
        [[nodiscard]] optional<VulkanQueue> &sparse_queue() noexcept;
        [[nodiscard]] optional<VulkanQueue> &video_decode_queue() noexcept;
        [[nodiscard]] optional<VulkanQueue> &video_encode_queue() noexcept;
        [[nodiscard]] vector<VulkanQueue> &graphics_queue_lanes() noexcept;
        [[nodiscard]] vector<VulkanQueue> &compute_queue_lanes() noexcept;
        [[nodiscard]] vector<VulkanQueue> &transfer_queue_lanes() noexcept;
        [[nodiscard]] vector<VulkanQueue> &sparse_queue_lanes() noexcept;
        [[nodiscard]] vector<VulkanQueue> &video_decode_queue_lanes() noexcept;
        [[nodiscard]] vector<VulkanQueue> &video_encode_queue_lanes() noexcept;

        void wait_idle() noexcept;

        void destroy() noexcept;

        // -------------------------------------------------------------------------
        // Memory
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkDeviceMemory> allocate_memory(const VkMemoryAllocateInfo &info) noexcept;

        void free_memory(VkDeviceMemory memory) noexcept;

        [[nodiscard]] RendererExpected<void *> map_memory(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags = 0) noexcept;

        void unmap_memory(VkDeviceMemory memory) noexcept;

        [[nodiscard]] RendererResult flush_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept;

        [[nodiscard]] RendererResult invalidate_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept;

        // -------------------------------------------------------------------------
        // Buffers
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkBuffer> create_buffer(const VkBufferCreateInfo &info) noexcept;

        void destroy_buffer(VkBuffer buffer) noexcept;

        [[nodiscard]] VkMemoryRequirements buffer_memory_requirements(VkBuffer buffer) const noexcept;

        [[nodiscard]] VkMemoryRequirements2 buffer_memory_requirements2(VkBuffer buffer) const noexcept;

        [[nodiscard]] RendererResult bind_buffer_memory(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset = 0) noexcept;

        // Requires VK_KHR_buffer_device_address / Vulkan 1.2.
        [[nodiscard]] VkDeviceAddress buffer_device_address(VkBuffer buffer) const noexcept;

        [[nodiscard]] u64 buffer_opaque_capture_address(VkBuffer buffer) const noexcept;

        // -------------------------------------------------------------------------
        // Images
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkImage> create_image(const VkImageCreateInfo &info) noexcept;

        void destroy_image(VkImage image) noexcept;

        [[nodiscard]] VkMemoryRequirements image_memory_requirements(VkImage image) const noexcept;

        [[nodiscard]] VkMemoryRequirements2 image_memory_requirements2(VkImage image) const noexcept;

        [[nodiscard]] RendererResult bind_image_memory(VkImage image, VkDeviceMemory memory, VkDeviceSize offset = 0) noexcept;

        [[nodiscard]] VkSubresourceLayout image_subresource_layout(VkImage image,
                                                                   const VkImageSubresource &subresource) const noexcept;

        // -------------------------------------------------------------------------
        // Image Views
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkImageView> create_image_view(const VkImageViewCreateInfo &info) noexcept;

        void destroy_image_view(VkImageView view) noexcept;

        // -------------------------------------------------------------------------
        // Samplers
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkSampler> create_sampler(const VkSamplerCreateInfo &info) noexcept;

        void destroy_sampler(VkSampler sampler) noexcept;

        // -------------------------------------------------------------------------
        // Shader Modules
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkShaderModule> create_shader_module(const VkShaderModuleCreateInfo &info) noexcept;

        void destroy_shader_module(VkShaderModule shader_module) noexcept;

        // -------------------------------------------------------------------------
        // Pipeline Layouts
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkPipelineLayout> create_pipeline_layout(const VkPipelineLayoutCreateInfo &info) noexcept;

        void destroy_pipeline_layout(VkPipelineLayout layout) noexcept;

        // -------------------------------------------------------------------------
        // Pipeline Caches
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkPipelineCache> create_pipeline_cache(const VkPipelineCacheCreateInfo &info) noexcept;

        void destroy_pipeline_cache(VkPipelineCache cache) noexcept;

        [[nodiscard]] RendererResult merge_pipeline_caches(VkPipelineCache dst,
                                                           span<const VkPipelineCache> srcs) noexcept;

        [[nodiscard]] RendererExpected<vector<u8>> pipeline_cache_data(VkPipelineCache cache) const;

        // -------------------------------------------------------------------------
        // Pipelines
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkPipeline> create_graphics_pipeline(
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept;

        [[nodiscard]] RendererExpected<vector<VkPipeline>> create_graphics_pipelines(
            VkPipelineCache cache,
            span<const VkGraphicsPipelineCreateInfo> infos);

        [[nodiscard]] RendererExpected<VkPipeline> create_compute_pipeline(
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept;

        [[nodiscard]] RendererExpected<vector<VkPipeline>> create_compute_pipelines(
            VkPipelineCache cache,
            span<const VkComputePipelineCreateInfo> infos);

        void destroy_pipeline(VkPipeline pipeline) noexcept;

        // -------------------------------------------------------------------------
        // Descriptor Set Layouts
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkDescriptorSetLayout> create_descriptor_set_layout(
            const VkDescriptorSetLayoutCreateInfo &info) noexcept;

        void destroy_descriptor_set_layout(VkDescriptorSetLayout layout) noexcept;

        // Queries whether a descriptor set layout can be created (Vulkan 1.1+).
        [[nodiscard]] VkDescriptorSetLayoutSupport descriptor_set_layout_support(
            const VkDescriptorSetLayoutCreateInfo &info) const noexcept;

        // -------------------------------------------------------------------------
        // Descriptor Pools
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkDescriptorPool> create_descriptor_pool(
            const VkDescriptorPoolCreateInfo &info) noexcept;

        void destroy_descriptor_pool(VkDescriptorPool pool) noexcept;

        [[nodiscard]] RendererResult reset_descriptor_pool(VkDescriptorPool pool,
                                                           VkDescriptorPoolResetFlags flags = 0) noexcept;

        // -------------------------------------------------------------------------
        // Descriptor Sets
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<vector<VkDescriptorSet>> allocate_descriptor_sets(
            const VkDescriptorSetAllocateInfo &info);

        [[nodiscard]] RendererResult free_descriptor_sets(VkDescriptorPool pool,
                                                          span<const VkDescriptorSet> sets) noexcept;

        void update_descriptor_sets(span<const VkWriteDescriptorSet> writes,
                                    span<const VkCopyDescriptorSet> copies) noexcept;

        // -------------------------------------------------------------------------
        // Command Pools
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkCommandPool> create_command_pool(
            const VkCommandPoolCreateInfo &info) noexcept;

        void destroy_command_pool(VkCommandPool pool) noexcept;

        [[nodiscard]] RendererResult reset_command_pool(VkCommandPool pool,
                                                        VkCommandPoolResetFlags flags = 0) noexcept;

        // Recycles unused memory from the pool back to the system (Vulkan 1.1+).
        void trim_command_pool(VkCommandPool pool, VkCommandPoolTrimFlags flags = 0) noexcept;

        // -------------------------------------------------------------------------
        // Command Buffers
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<vector<VkCommandBuffer>> allocate_command_buffers(
            const VkCommandBufferAllocateInfo &info);

        void free_command_buffers(VkCommandPool pool, span<const VkCommandBuffer> buffers) noexcept;

        [[nodiscard]] RendererResult reset_command_buffer(VkCommandBuffer buffer,
                                                          VkCommandBufferResetFlags flags = 0) noexcept;

        // -------------------------------------------------------------------------
        // Fences
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkFence> create_fence(const VkFenceCreateInfo &info) noexcept;

        void destroy_fence(VkFence fence) noexcept;

        [[nodiscard]] RendererResult reset_fences(span<const VkFence> fences) noexcept;

        // Returns success on both VK_SUCCESS and VK_TIMEOUT (caller checks elapsed time separately).
        [[nodiscard]] RendererResult wait_for_fences(span<const VkFence> fences, bool wait_all, u64 timeout_ns) noexcept;

        // Returns true if signaled, false if not ready.
        [[nodiscard]] RendererExpected<bool> is_fence_signaled(VkFence fence) const noexcept;

        // -------------------------------------------------------------------------
        // Semaphores
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkSemaphore> create_semaphore(const VkSemaphoreCreateInfo &info) noexcept;

        void destroy_semaphore(VkSemaphore semaphore) noexcept;

        // Timeline semaphore operations (Vulkan 1.2+).
        [[nodiscard]] RendererExpected<u64> semaphore_counter_value(VkSemaphore semaphore) const noexcept;

        [[nodiscard]] RendererResult signal_semaphore(const VkSemaphoreSignalInfo &info) noexcept;

        // Returns success on both VK_SUCCESS and VK_TIMEOUT.
        [[nodiscard]] RendererResult wait_semaphores(const VkSemaphoreWaitInfo &info, u64 timeout_ns) noexcept;

        // -------------------------------------------------------------------------
        // Events
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkEvent> create_event(const VkEventCreateInfo &info) noexcept;

        void destroy_event(VkEvent event) noexcept;

        // Returns true if the event is set, false if reset.
        [[nodiscard]] RendererExpected<bool> event_status(VkEvent event) const noexcept;

        [[nodiscard]] RendererResult set_event(VkEvent event) noexcept;

        [[nodiscard]] RendererResult reset_event(VkEvent event) noexcept;

        // -------------------------------------------------------------------------
        // Query Pools
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkQueryPool> create_query_pool(const VkQueryPoolCreateInfo &info) noexcept;

        void destroy_query_pool(VkQueryPool pool) noexcept;

        // VK_NOT_READY is not treated as an error — check result count or use VK_QUERY_RESULT_WAIT_BIT.
        [[nodiscard]] RendererResult get_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count, span<u8> data, VkDeviceSize stride, VkQueryResultFlags flags) noexcept;

        void reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) noexcept;

        // -------------------------------------------------------------------------
        // Swapchain (VK_KHR_swapchain)
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkSwapchainKHR> create_swapchain(
            const VkSwapchainCreateInfoKHR &info) noexcept;

        void destroy_swapchain(VkSwapchainKHR swapchain) noexcept;

        [[nodiscard]] RendererExpected<vector<VkImage>> swapchain_images(VkSwapchainKHR swapchain) const;

        // Returns VK_SUBOPTIMAL_KHR as success — caller should check the surface on next frame.
        [[nodiscard]] RendererExpected<u32> acquire_next_image(const VkAcquireNextImageInfoKHR &info) noexcept;

        // -------------------------------------------------------------------------
        // Debug Utils (VK_EXT_debug_utils) — no-op when extension is not loaded
        // -------------------------------------------------------------------------

        void set_debug_name(VkObjectType type, u64 object_handle, const char *name) noexcept;

        void set_debug_tag(VkObjectType type, u64 object_handle, u64 tag_name, span<const u8> tag_data) noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;

        optional<VulkanQueue> graphics_queue_{};
        optional<VulkanQueue> present_queue_{};
        optional<VulkanQueue> compute_queue_{};
        optional<VulkanQueue> transfer_queue_{};
        optional<VulkanQueue> sparse_queue_{};
        optional<VulkanQueue> video_decode_queue_{};
        optional<VulkanQueue> video_encode_queue_{};
        vector<VulkanQueue> graphics_queue_lanes_;
        vector<VulkanQueue> compute_queue_lanes_;
        vector<VulkanQueue> transfer_queue_lanes_;
        vector<VulkanQueue> sparse_queue_lanes_;
        vector<VulkanQueue> video_decode_queue_lanes_;
        vector<VulkanQueue> video_encode_queue_lanes_;
    };

} // namespace SFT::Core::Vulkan
