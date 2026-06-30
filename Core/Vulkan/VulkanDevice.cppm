module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <optional>
#include <span>
#include <vector>

export module Sturdy.Core:VulkanDevice;

import :RendererError;
import :VulkanQueue;
import Sturdy.Foundation;

using SFT::Core::renderer_error;
using SFT::Core::RendererErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;
using std::optional;
using std::span;
using std::vector;

export namespace SFT::Core::Vulkan {

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
            span<const char *> extensions{};
            // Optional pNext chain for VkDeviceCreateInfo — use to pass
            // VkPhysicalDeviceFeatures2 and any feature structs.
            const void *features_pnext = nullptr;
        };

        VulkanDevice() = default;

        ~VulkanDevice() { destroy(); }

        VulkanDevice(const VulkanDevice &) = delete;
        VulkanDevice &operator=(const VulkanDevice &) = delete;

        VulkanDevice(VulkanDevice &&o) noexcept
            : device_(o.device_), physical_device_(o.physical_device_),
              graphics_queue_(std::move(o.graphics_queue_)),
              present_queue_(std::move(o.present_queue_)),
              compute_queue_(std::move(o.compute_queue_)),
              transfer_queue_(std::move(o.transfer_queue_)) {
            o.device_ = VK_NULL_HANDLE;
            o.physical_device_ = VK_NULL_HANDLE;
        }

        VulkanDevice &operator=(VulkanDevice &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                physical_device_ = o.physical_device_;
                graphics_queue_ = std::move(o.graphics_queue_);
                present_queue_ = std::move(o.present_queue_);
                compute_queue_ = std::move(o.compute_queue_);
                transfer_queue_ = std::move(o.transfer_queue_);
                o.device_ = VK_NULL_HANDLE;
                o.physical_device_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // Creates a VkDevice from a pre-selected physical device + descriptor.
        // Deduplicates queue families automatically so the same index is not
        // requested twice (Vulkan spec requires unique family indices in pQueueCreateInfos).
        [[nodiscard]] static RendererExpected<VulkanDevice> create(
            VkPhysicalDevice physical,
            const DeviceCreateDesc &desc) noexcept {
            // Collect unique queue family indices.
            vector<u32> families;
            auto push_unique = [&](optional<u32> fam) {
                if (!fam)
                    return;
                for (u32 f : families)
                    if (f == *fam)
                        return;
                families.push_back(*fam);
            };
            push_unique(desc.graphics_queue_family);
            push_unique(desc.present_queue_family);
            push_unique(desc.compute_queue_family);
            push_unique(desc.transfer_queue_family);

            const float priority = 1.0f;
            vector<VkDeviceQueueCreateInfo> queue_infos;
            queue_infos.reserve(families.size());
            for (u32 fam : families) {
                queue_infos.push_back({
                    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    .pNext = nullptr,
                    .queueFamilyIndex = fam,
                    .queueCount = 1,
                    .pQueuePriorities = &priority,
                });
            }

            VkDeviceCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .pNext = desc.features_pnext,
                .queueCreateInfoCount = static_cast<u32>(queue_infos.size()),
                .pQueueCreateInfos = queue_infos.data(),
                .enabledExtensionCount = static_cast<u32>(desc.extensions.size()),
                .ppEnabledExtensionNames = desc.extensions.data(),
            };

            VkDevice vk_device = VK_NULL_HANDLE;
            if (vkCreateDevice(physical, &create_info, nullptr, &vk_device) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::InitializationFailed, "vkCreateDevice failed.");

            volkLoadDevice(vk_device);

            auto get_queue = [&](optional<u32> fam) -> optional<VulkanQueue> {
                if (!fam)
                    return {};
                VkQueue q = VK_NULL_HANDLE;
                vkGetDeviceQueue(vk_device, *fam, 0, &q);
                return VulkanQueue(q, *fam);
            };

            VulkanDevice out;
            out.device_ = vk_device;
            out.physical_device_ = physical;
            out.graphics_queue_ = get_queue(desc.graphics_queue_family);
            out.present_queue_ = get_queue(desc.present_queue_family);
            out.compute_queue_ = get_queue(desc.compute_queue_family);
            out.transfer_queue_ = get_queue(desc.transfer_queue_family);
            return out;
        }

        // -------------------------------------------------------------------------
        // Handle / validity
        // -------------------------------------------------------------------------

        [[nodiscard]] VkDevice vk_handle() const noexcept { return device_; }
        [[nodiscard]] VkPhysicalDevice physical_vk_handle() const noexcept { return physical_device_; }
        [[nodiscard]] bool is_valid() const noexcept { return device_ != VK_NULL_HANDLE; }

        [[nodiscard]] optional<VulkanQueue> &graphics_queue() noexcept { return graphics_queue_; }
        [[nodiscard]] optional<VulkanQueue> &present_queue() noexcept { return present_queue_; }
        [[nodiscard]] optional<VulkanQueue> &compute_queue() noexcept { return compute_queue_; }
        [[nodiscard]] optional<VulkanQueue> &transfer_queue() noexcept { return transfer_queue_; }

        void wait_idle() noexcept {
            if (device_ != VK_NULL_HANDLE)
                vkDeviceWaitIdle(device_);
        }

        void destroy() noexcept {
            if (device_ == VK_NULL_HANDLE)
                return;
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
            physical_device_ = VK_NULL_HANDLE;
            graphics_queue_.reset();
            present_queue_.reset();
            compute_queue_.reset();
            transfer_queue_.reset();
        }

        // -------------------------------------------------------------------------
        // Memory
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkDeviceMemory> allocate_memory(const VkMemoryAllocateInfo &info) noexcept {
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (vkAllocateMemory(device_, &info, nullptr, &mem) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OutOfMemory, "vkAllocateMemory failed.");
            return mem;
        }

        void free_memory(VkDeviceMemory memory) noexcept {
            vkFreeMemory(device_, memory, nullptr);
        }

        [[nodiscard]] RendererExpected<void *> map_memory(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags = 0) noexcept {
            void *ptr = nullptr;
            if (vkMapMemory(device_, memory, offset, size, flags, &ptr) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkMapMemory failed.");
            return ptr;
        }

        void unmap_memory(VkDeviceMemory memory) noexcept { vkUnmapMemory(device_, memory); }

        [[nodiscard]] RendererResult flush_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept {
            if (vkFlushMappedMemoryRanges(device_, static_cast<u32>(ranges.size()), ranges.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkFlushMappedMemoryRanges failed.");
            return {};
        }

        [[nodiscard]] RendererResult invalidate_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept {
            if (vkInvalidateMappedMemoryRanges(device_, static_cast<u32>(ranges.size()), ranges.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkInvalidateMappedMemoryRanges failed.");
            return {};
        }

        // -------------------------------------------------------------------------
        // Buffers
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkBuffer> create_buffer(const VkBufferCreateInfo &info) noexcept {
            VkBuffer buf = VK_NULL_HANDLE;
            if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateBuffer failed.");
            return buf;
        }

        void destroy_buffer(VkBuffer buffer) noexcept { vkDestroyBuffer(device_, buffer, nullptr); }

        [[nodiscard]] VkMemoryRequirements buffer_memory_requirements(VkBuffer buffer) const noexcept {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, buffer, &req);
            return req;
        }

        [[nodiscard]] VkMemoryRequirements2 buffer_memory_requirements2(VkBuffer buffer) const noexcept {
            VkBufferMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .buffer = buffer,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetBufferMemoryRequirements2(device_, &query, &req);
            return req;
        }

        [[nodiscard]] RendererResult bind_buffer_memory(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset = 0) noexcept {
            if (vkBindBufferMemory(device_, buffer, memory, offset) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkBindBufferMemory failed.");
            return {};
        }

        // Requires VK_KHR_buffer_device_address / Vulkan 1.2.
        [[nodiscard]] VkDeviceAddress buffer_device_address(VkBuffer buffer) const noexcept {
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer,
            };
            return vkGetBufferDeviceAddress(device_, &info);
        }

        [[nodiscard]] u64 buffer_opaque_capture_address(VkBuffer buffer) const noexcept {
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer,
            };
            return vkGetBufferOpaqueCaptureAddress(device_, &info);
        }

        // -------------------------------------------------------------------------
        // Images
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkImage> create_image(const VkImageCreateInfo &info) noexcept {
            VkImage img = VK_NULL_HANDLE;
            if (vkCreateImage(device_, &info, nullptr, &img) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateImage failed.");
            return img;
        }

        void destroy_image(VkImage image) noexcept { vkDestroyImage(device_, image, nullptr); }

        [[nodiscard]] VkMemoryRequirements image_memory_requirements(VkImage image) const noexcept {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_, image, &req);
            return req;
        }

        [[nodiscard]] VkMemoryRequirements2 image_memory_requirements2(VkImage image) const noexcept {
            VkImageMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .image = image,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetImageMemoryRequirements2(device_, &query, &req);
            return req;
        }

        [[nodiscard]] RendererResult bind_image_memory(VkImage image, VkDeviceMemory memory, VkDeviceSize offset = 0) noexcept {
            if (vkBindImageMemory(device_, image, memory, offset) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkBindImageMemory failed.");
            return {};
        }

        [[nodiscard]] VkSubresourceLayout image_subresource_layout(VkImage image,
                                                                   const VkImageSubresource &subresource) const noexcept {
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device_, image, &subresource, &layout);
            return layout;
        }

        // -------------------------------------------------------------------------
        // Image Views
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkImageView> create_image_view(const VkImageViewCreateInfo &info) noexcept {
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device_, &info, nullptr, &view) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateImageView failed.");
            return view;
        }

        void destroy_image_view(VkImageView view) noexcept { vkDestroyImageView(device_, view, nullptr); }

        // -------------------------------------------------------------------------
        // Samplers
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkSampler> create_sampler(const VkSamplerCreateInfo &info) noexcept {
            VkSampler sampler = VK_NULL_HANDLE;
            if (vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateSampler failed.");
            return sampler;
        }

        void destroy_sampler(VkSampler sampler) noexcept { vkDestroySampler(device_, sampler, nullptr); }

        // -------------------------------------------------------------------------
        // Shader Modules
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkShaderModule> create_shader_module(const VkShaderModuleCreateInfo &info) noexcept {
            VkShaderModule mod = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateShaderModule failed.");
            return mod;
        }

        void destroy_shader_module(VkShaderModule shader_module) noexcept {
            vkDestroyShaderModule(device_, shader_module, nullptr);
        }

        // -------------------------------------------------------------------------
        // Pipeline Layouts
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkPipelineLayout> create_pipeline_layout(const VkPipelineLayoutCreateInfo &info) noexcept {
            VkPipelineLayout layout = VK_NULL_HANDLE;
            if (vkCreatePipelineLayout(device_, &info, nullptr, &layout) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreatePipelineLayout failed.");
            return layout;
        }

        void destroy_pipeline_layout(VkPipelineLayout layout) noexcept {
            vkDestroyPipelineLayout(device_, layout, nullptr);
        }

        // -------------------------------------------------------------------------
        // Pipeline Caches
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkPipelineCache> create_pipeline_cache(const VkPipelineCacheCreateInfo &info) noexcept {
            VkPipelineCache cache = VK_NULL_HANDLE;
            if (vkCreatePipelineCache(device_, &info, nullptr, &cache) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreatePipelineCache failed.");
            return cache;
        }

        void destroy_pipeline_cache(VkPipelineCache cache) noexcept {
            vkDestroyPipelineCache(device_, cache, nullptr);
        }

        [[nodiscard]] RendererResult merge_pipeline_caches(VkPipelineCache dst,
                                                           span<const VkPipelineCache> srcs) noexcept {
            if (vkMergePipelineCaches(device_, dst, static_cast<u32>(srcs.size()), srcs.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkMergePipelineCaches failed.");
            return {};
        }

        [[nodiscard]] RendererExpected<vector<u8>> pipeline_cache_data(VkPipelineCache cache) const {
            usize size = 0;
            if (vkGetPipelineCacheData(device_, cache, &size, nullptr) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetPipelineCacheData (size) failed.");
            vector<u8> data(size);
            if (vkGetPipelineCacheData(device_, cache, &size, data.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetPipelineCacheData (read) failed.");
            return data;
        }

        // -------------------------------------------------------------------------
        // Pipelines
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkPipeline> create_graphics_pipeline(
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device_, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateGraphicsPipelines failed.");
            return pipeline;
        }

        [[nodiscard]] RendererExpected<vector<VkPipeline>> create_graphics_pipelines(
            VkPipelineCache cache,
            span<const VkGraphicsPipelineCreateInfo> infos) {
            vector<VkPipeline> pipelines(infos.size(), VK_NULL_HANDLE);
            if (vkCreateGraphicsPipelines(device_, cache, static_cast<u32>(infos.size()), infos.data(), nullptr, pipelines.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateGraphicsPipelines (batch) failed.");
            return pipelines;
        }

        [[nodiscard]] RendererExpected<VkPipeline> create_compute_pipeline(
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device_, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateComputePipelines failed.");
            return pipeline;
        }

        [[nodiscard]] RendererExpected<vector<VkPipeline>> create_compute_pipelines(
            VkPipelineCache cache,
            span<const VkComputePipelineCreateInfo> infos) {
            vector<VkPipeline> pipelines(infos.size(), VK_NULL_HANDLE);
            if (vkCreateComputePipelines(device_, cache, static_cast<u32>(infos.size()), infos.data(), nullptr, pipelines.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateComputePipelines (batch) failed.");
            return pipelines;
        }

        void destroy_pipeline(VkPipeline pipeline) noexcept { vkDestroyPipeline(device_, pipeline, nullptr); }

        // -------------------------------------------------------------------------
        // Descriptor Set Layouts
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkDescriptorSetLayout> create_descriptor_set_layout(
            const VkDescriptorSetLayoutCreateInfo &info) noexcept {
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateDescriptorSetLayout failed.");
            return layout;
        }

        void destroy_descriptor_set_layout(VkDescriptorSetLayout layout) noexcept {
            vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        }

        // Queries whether a descriptor set layout can be created (Vulkan 1.1+).
        [[nodiscard]] VkDescriptorSetLayoutSupport descriptor_set_layout_support(
            const VkDescriptorSetLayoutCreateInfo &info) const noexcept {
            VkDescriptorSetLayoutSupport support{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
                .pNext = nullptr};
            vkGetDescriptorSetLayoutSupport(device_, &info, &support);
            return support;
        }

        // -------------------------------------------------------------------------
        // Descriptor Pools
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkDescriptorPool> create_descriptor_pool(
            const VkDescriptorPoolCreateInfo &info) noexcept {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device_, &info, nullptr, &pool) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateDescriptorPool failed.");
            return pool;
        }

        void destroy_descriptor_pool(VkDescriptorPool pool) noexcept {
            vkDestroyDescriptorPool(device_, pool, nullptr);
        }

        [[nodiscard]] RendererResult reset_descriptor_pool(VkDescriptorPool pool,
                                                           VkDescriptorPoolResetFlags flags = 0) noexcept {
            if (vkResetDescriptorPool(device_, pool, flags) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetDescriptorPool failed.");
            return {};
        }

        // -------------------------------------------------------------------------
        // Descriptor Sets
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<vector<VkDescriptorSet>> allocate_descriptor_sets(
            const VkDescriptorSetAllocateInfo &info) {
            vector<VkDescriptorSet> sets(info.descriptorSetCount, VK_NULL_HANDLE);
            if (vkAllocateDescriptorSets(device_, &info, sets.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return sets;
        }

        [[nodiscard]] RendererResult free_descriptor_sets(VkDescriptorPool pool,
                                                          span<const VkDescriptorSet> sets) noexcept {
            if (vkFreeDescriptorSets(device_, pool, static_cast<u32>(sets.size()), sets.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkFreeDescriptorSets failed.");
            return {};
        }

        void update_descriptor_sets(span<const VkWriteDescriptorSet> writes,
                                    span<const VkCopyDescriptorSet> copies) noexcept {
            vkUpdateDescriptorSets(device_,
                                   static_cast<u32>(writes.size()),
                                   writes.data(),
                                   static_cast<u32>(copies.size()),
                                   copies.data());
        }

        // -------------------------------------------------------------------------
        // Command Pools
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkCommandPool> create_command_pool(
            const VkCommandPoolCreateInfo &info) noexcept {
            VkCommandPool pool = VK_NULL_HANDLE;
            if (vkCreateCommandPool(device_, &info, nullptr, &pool) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateCommandPool failed.");
            return pool;
        }

        void destroy_command_pool(VkCommandPool pool) noexcept {
            vkDestroyCommandPool(device_, pool, nullptr);
        }

        [[nodiscard]] RendererResult reset_command_pool(VkCommandPool pool,
                                                        VkCommandPoolResetFlags flags = 0) noexcept {
            if (vkResetCommandPool(device_, pool, flags) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetCommandPool failed.");
            return {};
        }

        // Recycles unused memory from the pool back to the system (Vulkan 1.1+).
        void trim_command_pool(VkCommandPool pool, VkCommandPoolTrimFlags flags = 0) noexcept {
            vkTrimCommandPool(device_, pool, flags);
        }

        // -------------------------------------------------------------------------
        // Command Buffers
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<vector<VkCommandBuffer>> allocate_command_buffers(
            const VkCommandBufferAllocateInfo &info) {
            vector<VkCommandBuffer> buffers(info.commandBufferCount, VK_NULL_HANDLE);
            if (vkAllocateCommandBuffers(device_, &info, buffers.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OutOfMemory, "vkAllocateCommandBuffers failed.");
            return buffers;
        }

        void free_command_buffers(VkCommandPool pool, span<const VkCommandBuffer> buffers) noexcept {
            vkFreeCommandBuffers(device_, pool, static_cast<u32>(buffers.size()), buffers.data());
        }

        [[nodiscard]] RendererResult reset_command_buffer(VkCommandBuffer buffer,
                                                          VkCommandBufferResetFlags flags = 0) noexcept {
            if (vkResetCommandBuffer(buffer, flags) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetCommandBuffer failed.");
            return {};
        }

        // -------------------------------------------------------------------------
        // Fences
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkFence> create_fence(const VkFenceCreateInfo &info) noexcept {
            VkFence fence = VK_NULL_HANDLE;
            if (vkCreateFence(device_, &info, nullptr, &fence) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateFence failed.");
            return fence;
        }

        void destroy_fence(VkFence fence) noexcept { vkDestroyFence(device_, fence, nullptr); }

        [[nodiscard]] RendererResult reset_fences(span<const VkFence> fences) noexcept {
            if (vkResetFences(device_, static_cast<u32>(fences.size()), fences.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetFences failed.");
            return {};
        }

        // Returns success on both VK_SUCCESS and VK_TIMEOUT (caller checks elapsed time separately).
        [[nodiscard]] RendererResult wait_for_fences(span<const VkFence> fences, bool wait_all, u64 timeout_ns) noexcept {
            VkResult res = vkWaitForFences(device_, static_cast<u32>(fences.size()), fences.data(), wait_all ? VK_TRUE : VK_FALSE, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return renderer_error(RendererErrorCode::OperationFailed, "vkWaitForFences failed.");
            return {};
        }

        // Returns true if signaled, false if not ready.
        [[nodiscard]] RendererExpected<bool> is_fence_signaled(VkFence fence) const noexcept {
            VkResult res = vkGetFenceStatus(device_, fence);
            if (res == VK_SUCCESS)
                return true;
            if (res == VK_NOT_READY)
                return false;
            return renderer_error(RendererErrorCode::OperationFailed, "vkGetFenceStatus failed.");
        }

        // -------------------------------------------------------------------------
        // Semaphores
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkSemaphore> create_semaphore(const VkSemaphoreCreateInfo &info) noexcept {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (vkCreateSemaphore(device_, &info, nullptr, &semaphore) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateSemaphore failed.");
            return semaphore;
        }

        void destroy_semaphore(VkSemaphore semaphore) noexcept {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }

        // Timeline semaphore operations (Vulkan 1.2+).
        [[nodiscard]] RendererExpected<u64> semaphore_counter_value(VkSemaphore semaphore) const noexcept {
            u64 value = 0;
            if (vkGetSemaphoreCounterValue(device_, semaphore, &value) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetSemaphoreCounterValue failed.");
            return value;
        }

        [[nodiscard]] RendererResult signal_semaphore(const VkSemaphoreSignalInfo &info) noexcept {
            if (vkSignalSemaphore(device_, &info) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkSignalSemaphore failed.");
            return {};
        }

        // Returns success on both VK_SUCCESS and VK_TIMEOUT.
        [[nodiscard]] RendererResult wait_semaphores(const VkSemaphoreWaitInfo &info, u64 timeout_ns) noexcept {
            VkResult res = vkWaitSemaphores(device_, &info, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return renderer_error(RendererErrorCode::OperationFailed, "vkWaitSemaphores failed.");
            return {};
        }

        // -------------------------------------------------------------------------
        // Events
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkEvent> create_event(const VkEventCreateInfo &info) noexcept {
            VkEvent event = VK_NULL_HANDLE;
            if (vkCreateEvent(device_, &info, nullptr, &event) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateEvent failed.");
            return event;
        }

        void destroy_event(VkEvent event) noexcept { vkDestroyEvent(device_, event, nullptr); }

        // Returns true if the event is set, false if reset.
        [[nodiscard]] RendererExpected<bool> event_status(VkEvent event) const noexcept {
            VkResult res = vkGetEventStatus(device_, event);
            if (res == VK_EVENT_SET)
                return true;
            if (res == VK_EVENT_RESET)
                return false;
            return renderer_error(RendererErrorCode::OperationFailed, "vkGetEventStatus failed.");
        }

        [[nodiscard]] RendererResult set_event(VkEvent event) noexcept {
            if (vkSetEvent(device_, event) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkSetEvent failed.");
            return {};
        }

        [[nodiscard]] RendererResult reset_event(VkEvent event) noexcept {
            if (vkResetEvent(device_, event) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkResetEvent failed.");
            return {};
        }

        // -------------------------------------------------------------------------
        // Query Pools
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkQueryPool> create_query_pool(const VkQueryPoolCreateInfo &info) noexcept {
            VkQueryPool pool = VK_NULL_HANDLE;
            if (vkCreateQueryPool(device_, &info, nullptr, &pool) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateQueryPool failed.");
            return pool;
        }

        void destroy_query_pool(VkQueryPool pool) noexcept { vkDestroyQueryPool(device_, pool, nullptr); }

        // VK_NOT_READY is not treated as an error — check result count or use VK_QUERY_RESULT_WAIT_BIT.
        [[nodiscard]] RendererResult get_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count, span<u8> data, VkDeviceSize stride, VkQueryResultFlags flags) noexcept {
            VkResult res = vkGetQueryPoolResults(device_, pool, first_query, query_count, data.size_bytes(), data.data(), stride, flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetQueryPoolResults failed.");
            return {};
        }

        void reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) noexcept {
            vkResetQueryPool(device_, pool, first_query, query_count);
        }

        // -------------------------------------------------------------------------
        // Swapchain (VK_KHR_swapchain)
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkSwapchainKHR> create_swapchain(
            const VkSwapchainCreateInfoKHR &info) noexcept {
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateSwapchainKHR failed.");
            return swapchain;
        }

        void destroy_swapchain(VkSwapchainKHR swapchain) noexcept {
            vkDestroySwapchainKHR(device_, swapchain, nullptr);
        }

        [[nodiscard]] RendererExpected<vector<VkImage>> swapchain_images(VkSwapchainKHR swapchain) const {
            u32 count = 0;
            if (vkGetSwapchainImagesKHR(device_, swapchain, &count, nullptr) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (count) failed.");
            vector<VkImage> images(count, VK_NULL_HANDLE);
            if (vkGetSwapchainImagesKHR(device_, swapchain, &count, images.data()) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (populate) failed.");
            return images;
        }

        // Returns VK_SUBOPTIMAL_KHR as success — caller should check the surface on next frame.
        [[nodiscard]] RendererExpected<u32> acquire_next_image(const VkAcquireNextImageInfoKHR &info) noexcept {
            u32 index = 0;
            VkResult res = vkAcquireNextImage2KHR(device_, &info, &index);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                return renderer_error(RendererErrorCode::OperationFailed, "vkAcquireNextImage2KHR failed.");
            return index;
        }

        // -------------------------------------------------------------------------
        // Render Passes (vkCreateRenderPass2 — Vulkan 1.2+)
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkRenderPass> create_render_pass(
            const VkRenderPassCreateInfo2 &info) noexcept {
            VkRenderPass rp = VK_NULL_HANDLE;
            if (vkCreateRenderPass2(device_, &info, nullptr, &rp) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateRenderPass2 failed.");
            return rp;
        }

        void destroy_render_pass(VkRenderPass render_pass) noexcept {
            vkDestroyRenderPass(device_, render_pass, nullptr);
        }

        // -------------------------------------------------------------------------
        // Framebuffers
        // -------------------------------------------------------------------------

        [[nodiscard]] RendererExpected<VkFramebuffer> create_framebuffer(
            const VkFramebufferCreateInfo &info) noexcept {
            VkFramebuffer fb = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(device_, &info, nullptr, &fb) != VK_SUCCESS)
                return renderer_error(RendererErrorCode::OperationFailed, "vkCreateFramebuffer failed.");
            return fb;
        }

        void destroy_framebuffer(VkFramebuffer framebuffer) noexcept {
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        }

        // -------------------------------------------------------------------------
        // Debug Utils (VK_EXT_debug_utils) — no-op when extension is not loaded
        // -------------------------------------------------------------------------

        void set_debug_name(VkObjectType type, u64 object_handle, const char *name) noexcept {
            if (!vkSetDebugUtilsObjectNameEXT)
                return;
            VkDebugUtilsObjectNameInfoEXT info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = type,
                .objectHandle = object_handle,
                .pObjectName = name,
            };
            vkSetDebugUtilsObjectNameEXT(device_, &info);
        }

        void set_debug_tag(VkObjectType type, u64 object_handle, u64 tag_name, span<const u8> tag_data) noexcept {
            if (!vkSetDebugUtilsObjectTagEXT)
                return;
            VkDebugUtilsObjectTagInfoEXT info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT,
                .pNext = nullptr,
                .objectType = type,
                .objectHandle = object_handle,
                .tagName = tag_name,
                .tagSize = tag_data.size_bytes(),
                .pTag = tag_data.data(),
            };
            vkSetDebugUtilsObjectTagEXT(device_, &info);
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;

        optional<VulkanQueue> graphics_queue_{};
        optional<VulkanQueue> present_queue_{};
        optional<VulkanQueue> compute_queue_{};
        optional<VulkanQueue> transfer_queue_{};
    };

} // namespace SFT::Core::Vulkan
