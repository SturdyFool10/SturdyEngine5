#include "VulkanDevice.hpp"

namespace SFT::Core::Vulkan {

VulkanDevice::~VulkanDevice() { destroy(); }

VulkanDevice::VulkanDevice(VulkanDevice &&o) noexcept
            : device_(o.device_), physical_device_(o.physical_device_),
              graphics_queue_(std::move(o.graphics_queue_)),
              present_queue_(std::move(o.present_queue_)),
              compute_queue_(std::move(o.compute_queue_)),
              transfer_queue_(std::move(o.transfer_queue_)),
              sparse_queue_(std::move(o.sparse_queue_)),
              video_decode_queue_(std::move(o.video_decode_queue_)),
              video_encode_queue_(std::move(o.video_encode_queue_)),
              graphics_queue_lanes_(std::move(o.graphics_queue_lanes_)),
              compute_queue_lanes_(std::move(o.compute_queue_lanes_)),
              transfer_queue_lanes_(std::move(o.transfer_queue_lanes_)),
              sparse_queue_lanes_(std::move(o.sparse_queue_lanes_)),
              video_decode_queue_lanes_(std::move(o.video_decode_queue_lanes_)),
              video_encode_queue_lanes_(std::move(o.video_encode_queue_lanes_)) {
            o.device_ = VK_NULL_HANDLE;
            o.physical_device_ = VK_NULL_HANDLE;
        }

VulkanDevice &VulkanDevice::operator=(VulkanDevice &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                physical_device_ = o.physical_device_;
                graphics_queue_ = std::move(o.graphics_queue_);
                present_queue_ = std::move(o.present_queue_);
                compute_queue_ = std::move(o.compute_queue_);
                transfer_queue_ = std::move(o.transfer_queue_);
                sparse_queue_ = std::move(o.sparse_queue_);
                video_decode_queue_ = std::move(o.video_decode_queue_);
                video_encode_queue_ = std::move(o.video_encode_queue_);
                graphics_queue_lanes_ = std::move(o.graphics_queue_lanes_);
                compute_queue_lanes_ = std::move(o.compute_queue_lanes_);
                transfer_queue_lanes_ = std::move(o.transfer_queue_lanes_);
                sparse_queue_lanes_ = std::move(o.sparse_queue_lanes_);
                video_decode_queue_lanes_ = std::move(o.video_decode_queue_lanes_);
                video_encode_queue_lanes_ = std::move(o.video_encode_queue_lanes_);
                o.device_ = VK_NULL_HANDLE;
                o.physical_device_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanDevice> VulkanDevice::create(
            VkPhysicalDevice physical,
            const VulkanDevice::DeviceCreateDesc &desc) noexcept {
            u32 family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
            vector<VkQueueFamilyProperties> family_properties(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, family_properties.data());

            struct FamilyRequest {
                u32 family = 0;
                u32 count = 1;
            };
            vector<FamilyRequest> families;
            auto max_count_for = [&](u32 family) noexcept -> u32 {
                return family < family_properties.size() ? family_properties[family].queueCount : 1u;
            };
            auto push_family = [&](optional<u32> fam, u32 requested_count) {
                if (!fam) {
                    return;
                }
                const u32 count = std::max(1u, std::min(requested_count, max_count_for(*fam)));
                auto it = std::ranges::find(families, *fam, &FamilyRequest::family);
                if (it == families.end()) {
                    families.push_back(FamilyRequest{*fam, count});
                } else {
                    it->count = std::max(it->count, count);
                }
            };
            push_family(desc.graphics_queue_family, desc.graphics_queue_count);
            push_family(desc.present_queue_family, 1);
            push_family(desc.compute_queue_family, desc.compute_queue_count);
            push_family(desc.transfer_queue_family, desc.transfer_queue_count);
            push_family(desc.sparse_queue_family, desc.sparse_queue_count);
            push_family(desc.video_decode_queue_family, desc.video_decode_queue_count);
            push_family(desc.video_encode_queue_family, desc.video_encode_queue_count);

            vector<vector<float>> priorities;
            priorities.reserve(families.size());
            vector<VkDeviceQueueCreateInfo> queue_infos;
            queue_infos.reserve(families.size());
            for (const FamilyRequest &family : families) {
                vector<float> family_priorities(family.count, 1.0f);
                priorities.push_back(std::move(family_priorities));
                queue_infos.push_back(VkDeviceQueueCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                    .pNext = nullptr,
                    .queueFamilyIndex = family.family,
                    .queueCount = family.count,
                    .pQueuePriorities = priorities.back().data(),
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
                return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "vkCreateDevice failed.");

            volkLoadDevice(vk_device);

            auto get_queue = [&](optional<u32> fam) -> optional<VulkanQueue> {
                if (!fam)
                    return {};
                VkQueue q = VK_NULL_HANDLE;
                vkGetDeviceQueue(vk_device, *fam, 0, &q);
                return VulkanQueue(q, *fam);
            };
            auto get_queue_lanes = [&](optional<u32> fam) -> vector<VulkanQueue> {
                vector<VulkanQueue> lanes;
                if (!fam) {
                    return lanes;
                }
                const auto it = std::ranges::find(families, *fam, &FamilyRequest::family);
                const u32 count = it == families.end() ? 1u : it->count;
                lanes.reserve(count);
                for (u32 i = 0; i < count; ++i) {
                    VkQueue q = VK_NULL_HANDLE;
                    vkGetDeviceQueue(vk_device, *fam, i, &q);
                    lanes.emplace_back(q, *fam);
                }
                return lanes;
            };

            VulkanDevice out;
            out.device_ = vk_device;
            out.physical_device_ = physical;
            out.graphics_queue_ = get_queue(desc.graphics_queue_family);
            out.present_queue_ = get_queue(desc.present_queue_family);
            out.compute_queue_ = get_queue(desc.compute_queue_family);
            out.transfer_queue_ = get_queue(desc.transfer_queue_family);
            out.sparse_queue_ = get_queue(desc.sparse_queue_family);
            out.video_decode_queue_ = get_queue(desc.video_decode_queue_family);
            out.video_encode_queue_ = get_queue(desc.video_encode_queue_family);
            out.graphics_queue_lanes_ = get_queue_lanes(desc.graphics_queue_family);
            out.compute_queue_lanes_ = get_queue_lanes(desc.compute_queue_family);
            out.transfer_queue_lanes_ = get_queue_lanes(desc.transfer_queue_family);
            out.sparse_queue_lanes_ = get_queue_lanes(desc.sparse_queue_family);
            out.video_decode_queue_lanes_ = get_queue_lanes(desc.video_decode_queue_family);
            out.video_encode_queue_lanes_ = get_queue_lanes(desc.video_encode_queue_family);
            return out;
        }

[[nodiscard]] VkDevice VulkanDevice::vk_handle() const noexcept { return device_; }

[[nodiscard]] VkPhysicalDevice VulkanDevice::physical_vk_handle() const noexcept { return physical_device_; }

[[nodiscard]] bool VulkanDevice::is_valid() const noexcept { return device_ != VK_NULL_HANDLE; }

[[nodiscard]] optional<VulkanQueue> &VulkanDevice::graphics_queue() noexcept { return graphics_queue_; }

[[nodiscard]] optional<VulkanQueue> &VulkanDevice::present_queue() noexcept { return present_queue_; }

[[nodiscard]] optional<VulkanQueue> &VulkanDevice::compute_queue() noexcept { return compute_queue_; }

[[nodiscard]] optional<VulkanQueue> &VulkanDevice::transfer_queue() noexcept { return transfer_queue_; }

[[nodiscard]] optional<VulkanQueue> &VulkanDevice::sparse_queue() noexcept { return sparse_queue_; }

[[nodiscard]] optional<VulkanQueue> &VulkanDevice::video_decode_queue() noexcept { return video_decode_queue_; }

[[nodiscard]] optional<VulkanQueue> &VulkanDevice::video_encode_queue() noexcept { return video_encode_queue_; }

[[nodiscard]] vector<VulkanQueue> &VulkanDevice::graphics_queue_lanes() noexcept { return graphics_queue_lanes_; }

[[nodiscard]] vector<VulkanQueue> &VulkanDevice::compute_queue_lanes() noexcept { return compute_queue_lanes_; }

[[nodiscard]] vector<VulkanQueue> &VulkanDevice::transfer_queue_lanes() noexcept { return transfer_queue_lanes_; }

[[nodiscard]] vector<VulkanQueue> &VulkanDevice::sparse_queue_lanes() noexcept { return sparse_queue_lanes_; }

[[nodiscard]] vector<VulkanQueue> &VulkanDevice::video_decode_queue_lanes() noexcept { return video_decode_queue_lanes_; }

[[nodiscard]] vector<VulkanQueue> &VulkanDevice::video_encode_queue_lanes() noexcept { return video_encode_queue_lanes_; }

void VulkanDevice::wait_idle() noexcept {
            if (device_ != VK_NULL_HANDLE)
                vkDeviceWaitIdle(device_);
        }

void VulkanDevice::destroy() noexcept {
            if (device_ == VK_NULL_HANDLE)
                return;
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
            physical_device_ = VK_NULL_HANDLE;
            graphics_queue_.reset();
            present_queue_.reset();
            compute_queue_.reset();
            transfer_queue_.reset();
            sparse_queue_.reset();
            video_decode_queue_.reset();
            video_encode_queue_.reset();
            graphics_queue_lanes_.clear();
            compute_queue_lanes_.clear();
            transfer_queue_lanes_.clear();
            sparse_queue_lanes_.clear();
            video_decode_queue_lanes_.clear();
            video_encode_queue_lanes_.clear();
        }

[[nodiscard]] RendererExpected<VkDeviceMemory> VulkanDevice::allocate_memory(const VkMemoryAllocateInfo &info) noexcept {
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (vkAllocateMemory(device_, &info, nullptr, &mem) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateMemory failed.");
            return mem;
        }

void VulkanDevice::free_memory(VkDeviceMemory memory) noexcept {
            vkFreeMemory(device_, memory, nullptr);
        }

[[nodiscard]] RendererExpected<void *> VulkanDevice::map_memory(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags) noexcept {
            void *ptr = nullptr;
            if (vkMapMemory(device_, memory, offset, size, flags, &ptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkMapMemory failed.");
            return ptr;
        }

void VulkanDevice::unmap_memory(VkDeviceMemory memory) noexcept { vkUnmapMemory(device_, memory); }

[[nodiscard]] RendererResult VulkanDevice::flush_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept {
            if (vkFlushMappedMemoryRanges(device_, static_cast<u32>(ranges.size()), ranges.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkFlushMappedMemoryRanges failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanDevice::invalidate_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept {
            if (vkInvalidateMappedMemoryRanges(device_, static_cast<u32>(ranges.size()), ranges.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkInvalidateMappedMemoryRanges failed.");
            return {};
        }

[[nodiscard]] RendererExpected<VkBuffer> VulkanDevice::create_buffer(const VkBufferCreateInfo &info) noexcept {
            VkBuffer buf = VK_NULL_HANDLE;
            if (vkCreateBuffer(device_, &info, nullptr, &buf) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateBuffer failed.");
            return buf;
        }

void VulkanDevice::destroy_buffer(VkBuffer buffer) noexcept { vkDestroyBuffer(device_, buffer, nullptr); }

[[nodiscard]] VkMemoryRequirements VulkanDevice::buffer_memory_requirements(VkBuffer buffer) const noexcept {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, buffer, &req);
            return req;
        }

[[nodiscard]] VkMemoryRequirements2 VulkanDevice::buffer_memory_requirements2(VkBuffer buffer) const noexcept {
            VkBufferMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .buffer = buffer,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetBufferMemoryRequirements2(device_, &query, &req);
            return req;
        }

[[nodiscard]] RendererResult VulkanDevice::bind_buffer_memory(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) noexcept {
            if (vkBindBufferMemory(device_, buffer, memory, offset) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBindBufferMemory failed.");
            return {};
        }

[[nodiscard]] VkDeviceAddress VulkanDevice::buffer_device_address(VkBuffer buffer) const noexcept {
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer,
            };
            return vkGetBufferDeviceAddress(device_, &info);
        }

[[nodiscard]] u64 VulkanDevice::buffer_opaque_capture_address(VkBuffer buffer) const noexcept {
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer,
            };
            return vkGetBufferOpaqueCaptureAddress(device_, &info);
        }

[[nodiscard]] RendererExpected<VkImage> VulkanDevice::create_image(const VkImageCreateInfo &info) noexcept {
            VkImage img = VK_NULL_HANDLE;
            if (vkCreateImage(device_, &info, nullptr, &img) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateImage failed.");
            return img;
        }

void VulkanDevice::destroy_image(VkImage image) noexcept { vkDestroyImage(device_, image, nullptr); }

[[nodiscard]] VkMemoryRequirements VulkanDevice::image_memory_requirements(VkImage image) const noexcept {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_, image, &req);
            return req;
        }

[[nodiscard]] VkMemoryRequirements2 VulkanDevice::image_memory_requirements2(VkImage image) const noexcept {
            VkImageMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .image = image,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetImageMemoryRequirements2(device_, &query, &req);
            return req;
        }

[[nodiscard]] RendererResult VulkanDevice::bind_image_memory(VkImage image, VkDeviceMemory memory, VkDeviceSize offset) noexcept {
            if (vkBindImageMemory(device_, image, memory, offset) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBindImageMemory failed.");
            return {};
        }

[[nodiscard]] VkSubresourceLayout VulkanDevice::image_subresource_layout(VkImage image,
                                                                   const VkImageSubresource &subresource) const noexcept {
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device_, image, &subresource, &layout);
            return layout;
        }

[[nodiscard]] RendererExpected<VkImageView> VulkanDevice::create_image_view(const VkImageViewCreateInfo &info) noexcept {
            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device_, &info, nullptr, &view) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateImageView failed.");
            return view;
        }

void VulkanDevice::destroy_image_view(VkImageView view) noexcept { vkDestroyImageView(device_, view, nullptr); }

[[nodiscard]] RendererExpected<VkSampler> VulkanDevice::create_sampler(const VkSamplerCreateInfo &info) noexcept {
            VkSampler sampler = VK_NULL_HANDLE;
            if (vkCreateSampler(device_, &info, nullptr, &sampler) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateSampler failed.");
            return sampler;
        }

void VulkanDevice::destroy_sampler(VkSampler sampler) noexcept { vkDestroySampler(device_, sampler, nullptr); }

[[nodiscard]] RendererExpected<VkShaderModule> VulkanDevice::create_shader_module(const VkShaderModuleCreateInfo &info) noexcept {
            VkShaderModule mod = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device_, &info, nullptr, &mod) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateShaderModule failed.");
            return mod;
        }

void VulkanDevice::destroy_shader_module(VkShaderModule shader_module) noexcept {
            vkDestroyShaderModule(device_, shader_module, nullptr);
        }

[[nodiscard]] RendererExpected<VkPipelineLayout> VulkanDevice::create_pipeline_layout(const VkPipelineLayoutCreateInfo &info) noexcept {
            VkPipelineLayout layout = VK_NULL_HANDLE;
            if (vkCreatePipelineLayout(device_, &info, nullptr, &layout) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreatePipelineLayout failed.");
            return layout;
        }

void VulkanDevice::destroy_pipeline_layout(VkPipelineLayout layout) noexcept {
            vkDestroyPipelineLayout(device_, layout, nullptr);
        }

[[nodiscard]] RendererExpected<VkPipelineCache> VulkanDevice::create_pipeline_cache(const VkPipelineCacheCreateInfo &info) noexcept {
            VkPipelineCache cache = VK_NULL_HANDLE;
            if (vkCreatePipelineCache(device_, &info, nullptr, &cache) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreatePipelineCache failed.");
            return cache;
        }

void VulkanDevice::destroy_pipeline_cache(VkPipelineCache cache) noexcept {
            vkDestroyPipelineCache(device_, cache, nullptr);
        }

[[nodiscard]] RendererResult VulkanDevice::merge_pipeline_caches(VkPipelineCache dst,
                                                           span<const VkPipelineCache> srcs) noexcept {
            if (vkMergePipelineCaches(device_, dst, static_cast<u32>(srcs.size()), srcs.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkMergePipelineCaches failed.");
            return {};
        }

[[nodiscard]] RendererExpected<vector<u8>> VulkanDevice::pipeline_cache_data(VkPipelineCache cache) const {
            usize size = 0;
            if (vkGetPipelineCacheData(device_, cache, &size, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (size) failed.");
            vector<u8> data(size);
            if (vkGetPipelineCacheData(device_, cache, &size, data.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetPipelineCacheData (read) failed.");
            return data;
        }

[[nodiscard]] RendererExpected<VkPipeline> VulkanDevice::create_graphics_pipeline(
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device_, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines failed.");
            return pipeline;
        }

[[nodiscard]] RendererExpected<vector<VkPipeline>> VulkanDevice::create_graphics_pipelines(
            VkPipelineCache cache,
            span<const VkGraphicsPipelineCreateInfo> infos) {
            vector<VkPipeline> pipelines(infos.size(), VK_NULL_HANDLE);
            if (vkCreateGraphicsPipelines(device_, cache, static_cast<u32>(infos.size()), infos.data(), nullptr, pipelines.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateGraphicsPipelines (batch) failed.");
            return pipelines;
        }

[[nodiscard]] RendererExpected<VkPipeline> VulkanDevice::create_compute_pipeline(
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device_, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateComputePipelines failed.");
            return pipeline;
        }

[[nodiscard]] RendererExpected<vector<VkPipeline>> VulkanDevice::create_compute_pipelines(
            VkPipelineCache cache,
            span<const VkComputePipelineCreateInfo> infos) {
            vector<VkPipeline> pipelines(infos.size(), VK_NULL_HANDLE);
            if (vkCreateComputePipelines(device_, cache, static_cast<u32>(infos.size()), infos.data(), nullptr, pipelines.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateComputePipelines (batch) failed.");
            return pipelines;
        }

void VulkanDevice::destroy_pipeline(VkPipeline pipeline) noexcept { vkDestroyPipeline(device_, pipeline, nullptr); }

[[nodiscard]] RendererExpected<VkDescriptorSetLayout> VulkanDevice::create_descriptor_set_layout(
            const VkDescriptorSetLayoutCreateInfo &info) noexcept {
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateDescriptorSetLayout failed.");
            return layout;
        }

void VulkanDevice::destroy_descriptor_set_layout(VkDescriptorSetLayout layout) noexcept {
            vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        }

[[nodiscard]] VkDescriptorSetLayoutSupport VulkanDevice::descriptor_set_layout_support(
            const VkDescriptorSetLayoutCreateInfo &info) const noexcept {
            VkDescriptorSetLayoutSupport support{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
                .pNext = nullptr};
            vkGetDescriptorSetLayoutSupport(device_, &info, &support);
            return support;
        }

[[nodiscard]] RendererExpected<VkDescriptorPool> VulkanDevice::create_descriptor_pool(
            const VkDescriptorPoolCreateInfo &info) noexcept {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(device_, &info, nullptr, &pool) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateDescriptorPool failed.");
            return pool;
        }

void VulkanDevice::destroy_descriptor_pool(VkDescriptorPool pool) noexcept {
            vkDestroyDescriptorPool(device_, pool, nullptr);
        }

[[nodiscard]] RendererResult VulkanDevice::reset_descriptor_pool(VkDescriptorPool pool,
                                                           VkDescriptorPoolResetFlags flags) noexcept {
            if (vkResetDescriptorPool(device_, pool, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetDescriptorPool failed.");
            return {};
        }

[[nodiscard]] RendererExpected<vector<VkDescriptorSet>> VulkanDevice::allocate_descriptor_sets(
            const VkDescriptorSetAllocateInfo &info) {
            vector<VkDescriptorSet> sets(info.descriptorSetCount, VK_NULL_HANDLE);
            if (vkAllocateDescriptorSets(device_, &info, sets.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateDescriptorSets failed.");
            return sets;
        }

[[nodiscard]] RendererResult VulkanDevice::free_descriptor_sets(VkDescriptorPool pool,
                                                          span<const VkDescriptorSet> sets) noexcept {
            if (vkFreeDescriptorSets(device_, pool, static_cast<u32>(sets.size()), sets.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkFreeDescriptorSets failed.");
            return {};
        }

void VulkanDevice::update_descriptor_sets(span<const VkWriteDescriptorSet> writes,
                                    span<const VkCopyDescriptorSet> copies) noexcept {
            vkUpdateDescriptorSets(device_,
                                   static_cast<u32>(writes.size()),
                                   writes.data(),
                                   static_cast<u32>(copies.size()),
                                   copies.data());
        }

[[nodiscard]] RendererExpected<VkCommandPool> VulkanDevice::create_command_pool(
            const VkCommandPoolCreateInfo &info) noexcept {
            VkCommandPool pool = VK_NULL_HANDLE;
            if (vkCreateCommandPool(device_, &info, nullptr, &pool) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateCommandPool failed.");
            return pool;
        }

void VulkanDevice::destroy_command_pool(VkCommandPool pool) noexcept {
            vkDestroyCommandPool(device_, pool, nullptr);
        }

[[nodiscard]] RendererResult VulkanDevice::reset_command_pool(VkCommandPool pool,
                                                        VkCommandPoolResetFlags flags) noexcept {
            if (vkResetCommandPool(device_, pool, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetCommandPool failed.");
            return {};
        }

void VulkanDevice::trim_command_pool(VkCommandPool pool, VkCommandPoolTrimFlags flags) noexcept {
            vkTrimCommandPool(device_, pool, flags);
        }

[[nodiscard]] RendererExpected<vector<VkCommandBuffer>> VulkanDevice::allocate_command_buffers(
            const VkCommandBufferAllocateInfo &info) {
            vector<VkCommandBuffer> buffers(info.commandBufferCount, VK_NULL_HANDLE);
            if (vkAllocateCommandBuffers(device_, &info, buffers.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, "vkAllocateCommandBuffers failed.");
            return buffers;
        }

void VulkanDevice::free_command_buffers(VkCommandPool pool, span<const VkCommandBuffer> buffers) noexcept {
            vkFreeCommandBuffers(device_, pool, static_cast<u32>(buffers.size()), buffers.data());
        }

[[nodiscard]] RendererResult VulkanDevice::reset_command_buffer(VkCommandBuffer buffer,
                                                          VkCommandBufferResetFlags flags) noexcept {
            if (vkResetCommandBuffer(buffer, flags) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetCommandBuffer failed.");
            return {};
        }

[[nodiscard]] RendererExpected<VkFence> VulkanDevice::create_fence(const VkFenceCreateInfo &info) noexcept {
            VkFence fence = VK_NULL_HANDLE;
            if (vkCreateFence(device_, &info, nullptr, &fence) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateFence failed.");
            return fence;
        }

void VulkanDevice::destroy_fence(VkFence fence) noexcept { vkDestroyFence(device_, fence, nullptr); }

[[nodiscard]] RendererResult VulkanDevice::reset_fences(span<const VkFence> fences) noexcept {
            if (vkResetFences(device_, static_cast<u32>(fences.size()), fences.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetFences failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanDevice::wait_for_fences(span<const VkFence> fences, bool wait_all, u64 timeout_ns) noexcept {
            VkResult res = vkWaitForFences(device_, static_cast<u32>(fences.size()), fences.data(), wait_all ? VK_TRUE : VK_FALSE, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkWaitForFences failed.");
            return {};
        }

[[nodiscard]] RendererExpected<bool> VulkanDevice::is_fence_signaled(VkFence fence) const noexcept {
            VkResult res = vkGetFenceStatus(device_, fence);
            if (res == VK_SUCCESS)
                return true;
            if (res == VK_NOT_READY)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetFenceStatus failed.");
        }

[[nodiscard]] RendererExpected<VkSemaphore> VulkanDevice::create_semaphore(const VkSemaphoreCreateInfo &info) noexcept {
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (vkCreateSemaphore(device_, &info, nullptr, &semaphore) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateSemaphore failed.");
            return semaphore;
        }

void VulkanDevice::destroy_semaphore(VkSemaphore semaphore) noexcept {
            vkDestroySemaphore(device_, semaphore, nullptr);
        }

[[nodiscard]] RendererExpected<u64> VulkanDevice::semaphore_counter_value(VkSemaphore semaphore) const noexcept {
            u64 value = 0;
            if (vkGetSemaphoreCounterValue(device_, semaphore, &value) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetSemaphoreCounterValue failed.");
            return value;
        }

[[nodiscard]] RendererResult VulkanDevice::signal_semaphore(const VkSemaphoreSignalInfo &info) noexcept {
            if (vkSignalSemaphore(device_, &info) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkSignalSemaphore failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanDevice::wait_semaphores(const VkSemaphoreWaitInfo &info, u64 timeout_ns) noexcept {
            VkResult res = vkWaitSemaphores(device_, &info, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkWaitSemaphores failed.");
            return {};
        }

[[nodiscard]] RendererExpected<VkEvent> VulkanDevice::create_event(const VkEventCreateInfo &info) noexcept {
            VkEvent event = VK_NULL_HANDLE;
            if (vkCreateEvent(device_, &info, nullptr, &event) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateEvent failed.");
            return event;
        }

void VulkanDevice::destroy_event(VkEvent event) noexcept { vkDestroyEvent(device_, event, nullptr); }

[[nodiscard]] RendererExpected<bool> VulkanDevice::event_status(VkEvent event) const noexcept {
            VkResult res = vkGetEventStatus(device_, event);
            if (res == VK_EVENT_SET)
                return true;
            if (res == VK_EVENT_RESET)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetEventStatus failed.");
        }

[[nodiscard]] RendererResult VulkanDevice::set_event(VkEvent event) noexcept {
            if (vkSetEvent(device_, event) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkSetEvent failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanDevice::reset_event(VkEvent event) noexcept {
            if (vkResetEvent(device_, event) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkResetEvent failed.");
            return {};
        }

[[nodiscard]] RendererExpected<VkQueryPool> VulkanDevice::create_query_pool(const VkQueryPoolCreateInfo &info) noexcept {
            VkQueryPool pool = VK_NULL_HANDLE;
            if (vkCreateQueryPool(device_, &info, nullptr, &pool) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateQueryPool failed.");
            return pool;
        }

void VulkanDevice::destroy_query_pool(VkQueryPool pool) noexcept { vkDestroyQueryPool(device_, pool, nullptr); }

[[nodiscard]] RendererResult VulkanDevice::get_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count, span<u8> data, VkDeviceSize stride, VkQueryResultFlags flags) noexcept {
            VkResult res = vkGetQueryPoolResults(device_, pool, first_query, query_count, data.size_bytes(), data.data(), stride, flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetQueryPoolResults failed.");
            return {};
        }

void VulkanDevice::reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) noexcept {
            vkResetQueryPool(device_, pool, first_query, query_count);
        }

[[nodiscard]] RendererExpected<VkSwapchainKHR> VulkanDevice::create_swapchain(
            const VkSwapchainCreateInfoKHR &info) noexcept {
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateSwapchainKHR failed.");
            return swapchain;
        }

void VulkanDevice::destroy_swapchain(VkSwapchainKHR swapchain) noexcept {
            vkDestroySwapchainKHR(device_, swapchain, nullptr);
        }

[[nodiscard]] RendererExpected<vector<VkImage>> VulkanDevice::swapchain_images(VkSwapchainKHR swapchain) const {
            u32 count = 0;
            if (vkGetSwapchainImagesKHR(device_, swapchain, &count, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (count) failed.");
            vector<VkImage> images(count, VK_NULL_HANDLE);
            if (vkGetSwapchainImagesKHR(device_, swapchain, &count, images.data()) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkGetSwapchainImagesKHR (populate) failed.");
            return images;
        }

[[nodiscard]] RendererExpected<u32> VulkanDevice::acquire_next_image(const VkAcquireNextImageInfoKHR &info) noexcept {
            u32 index = 0;
            VkResult res = vkAcquireNextImage2KHR(device_, &info, &index);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkAcquireNextImage2KHR failed.");
            return index;
        }

void VulkanDevice::set_debug_name(VkObjectType type, u64 object_handle, const char *name) noexcept {
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

void VulkanDevice::set_debug_tag(VkObjectType type, u64 object_handle, u64 tag_name, span<const u8> tag_data) noexcept {
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

} // namespace SFT::Core::Vulkan
