#include <Core/Vulkan/VulkanDevice.hpp>

#include <Core/Vulkan/VulkanHelpers.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanDevice::~VulkanDevice() { destroy(); }

/// Performs the vulkan device operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
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
            ZoneScopedN("VulkanDevice::VulkanDevice");
            o.device_ = VK_NULL_HANDLE;
            o.physical_device_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanDevice &VulkanDevice::operator=(VulkanDevice &&o) noexcept {
            ZoneScopedN("VulkanDevice::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param physical `physical` value used by the operation.
/// @param desc Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanDevice> VulkanDevice::create(
            VkPhysicalDevice physical,
            const VulkanDevice::DeviceCreateDesc &desc) noexcept {
            ZoneScopedN("VulkanDevice::create");
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
            if (const VkResult result = vkCreateDevice(physical, &create_info, nullptr, &vk_device);
                result != VK_SUCCESS) {
                return graphics_backend_error(
                    GraphicsBackendErrorCode::InitializationFailed,
                    format("vkCreateDevice failed: {} ({} extensions requested: {})",
                           vulkan_result_name(result), desc.extensions.size(),
                           [&] {
                               string joined;
                               for (usize i = 0; i < desc.extensions.size(); ++i) {
                                   if (i > 0) joined += ", ";
                                   joined += desc.extensions[i];
                               }
                               return joined;
                           }()));
            }

            volkLoadDevice(vk_device);

            auto get_queue = [&](optional<u32> fam, u32 index = 0) -> optional<VulkanQueue> {
                if (!fam)
                    return {};
                const auto family = std::ranges::find(families, *fam, &FamilyRequest::family);
                if (family == families.end() || index >= family->count) {
                    return {};
                }
                VkQueue q = VK_NULL_HANDLE;
                vkGetDeviceQueue(vk_device, *fam, index, &q);
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
            out.present_queue_ = get_queue(desc.present_queue_family, desc.present_queue_index);
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDevice VulkanDevice::vk_handle() const noexcept { return device_; }

/// Returns the physical Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current physical Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkPhysicalDevice VulkanDevice::physical_vk_handle() const noexcept { return physical_device_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanDevice::is_valid() const noexcept { return device_ != VK_NULL_HANDLE; }

/// Returns the current or globally available graphics queue value.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<VulkanQueue> &VulkanDevice::graphics_queue() noexcept { return graphics_queue_; }

/// Presents the completed frame to the target surface or swapchain.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<VulkanQueue> &VulkanDevice::present_queue() noexcept { return present_queue_; }

/// Computes queue using the supplied arguments and current state.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<VulkanQueue> &VulkanDevice::compute_queue() noexcept { return compute_queue_; }

/// Returns the current or globally available transfer queue value.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<VulkanQueue> &VulkanDevice::transfer_queue() noexcept { return transfer_queue_; }

/// Returns the current or globally available sparse queue value.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<VulkanQueue> &VulkanDevice::sparse_queue() noexcept { return sparse_queue_; }

/// Returns the current or globally available video decode queue value.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<VulkanQueue> &VulkanDevice::video_decode_queue() noexcept { return video_decode_queue_; }

/// Returns the current or globally available video encode queue value.
///
/// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
/// @note This function does not throw exceptions.
[[nodiscard]] optional<VulkanQueue> &VulkanDevice::video_encode_queue() noexcept { return video_encode_queue_; }

/// Returns the current or globally available graphics queue lanes value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VulkanQueue> &VulkanDevice::graphics_queue_lanes() noexcept { return graphics_queue_lanes_; }

/// Computes queue lanes using the supplied arguments and current state.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VulkanQueue> &VulkanDevice::compute_queue_lanes() noexcept { return compute_queue_lanes_; }

/// Returns the current or globally available transfer queue lanes value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VulkanQueue> &VulkanDevice::transfer_queue_lanes() noexcept { return transfer_queue_lanes_; }

/// Returns the current or globally available sparse queue lanes value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VulkanQueue> &VulkanDevice::sparse_queue_lanes() noexcept { return sparse_queue_lanes_; }

/// Returns the current or globally available video decode queue lanes value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VulkanQueue> &VulkanDevice::video_decode_queue_lanes() noexcept { return video_decode_queue_lanes_; }

/// Returns the current or globally available video encode queue lanes value.
///
/// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] vector<VulkanQueue> &VulkanDevice::video_encode_queue_lanes() noexcept { return video_encode_queue_lanes_; }

/// Waits for idle to complete.
///
/// @return Returns the current wait idle value.
/// @note This function does not throw exceptions.
void VulkanDevice::wait_idle() noexcept {
            ZoneScopedN("VulkanDevice::wait_idle");
            if (device_ != VK_NULL_HANDLE)
                vkDeviceWaitIdle(device_);
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy() noexcept {
            ZoneScopedN("VulkanDevice::destroy");
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

/// Allocates memory.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkDeviceMemory> VulkanDevice::allocate_memory(const VkMemoryAllocateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::allocate_memory");
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (const VkResult result = vkAllocateMemory(device_, &info, nullptr, &mem); result != VK_SUCCESS)
                return graphics_backend_error(
                    result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? GraphicsBackendErrorCode::OutOfMemory
                                                                                                     : GraphicsBackendErrorCode::OperationFailed,
                    vulkan_failure_message("vkAllocateMemory", result,
                                           std::format("requested {} bytes from memory type {}", info.allocationSize, info.memoryTypeIndex)));
            return mem;
        }

/// Releases previously allocated storage or resources.
///
/// @param memory `memory` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::free_memory(VkDeviceMemory memory) noexcept {
            ZoneScopedN("VulkanDevice::free_memory");
            vkFreeMemory(device_, memory, nullptr);
        }

/// Maps memory for access.
///
/// @param memory `memory` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<void *> VulkanDevice::map_memory(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags) noexcept {
            ZoneScopedN("VulkanDevice::map_memory");
            void *ptr = nullptr;
            if (const VkResult result = vkMapMemory(device_, memory, offset, size, flags, &ptr); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkMapMemory", result));
            return ptr;
        }

/// Unmaps memory.
///
/// @param memory `memory` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::unmap_memory(VkDeviceMemory memory) noexcept { vkUnmapMemory(device_, memory); }

/// Flushes mapped memory ranges.
///
/// @param ranges `ranges` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::flush_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept {
            ZoneScopedN("VulkanDevice::flush_mapped_memory_ranges");
            if (const VkResult result = vkFlushMappedMemoryRanges(device_, static_cast<u32>(ranges.size()), ranges.data()); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkFlushMappedMemoryRanges", result));
            return {};
        }

/// Performs the invalidate mapped memory ranges operation for `Vulkan` using the supplied arguments.
///
/// @param ranges `ranges` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::invalidate_mapped_memory_ranges(span<const VkMappedMemoryRange> ranges) noexcept {
            ZoneScopedN("VulkanDevice::invalidate_mapped_memory_ranges");
            if (const VkResult result = vkInvalidateMappedMemoryRanges(device_, static_cast<u32>(ranges.size()), ranges.data()); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkInvalidateMappedMemoryRanges", result));
            return {};
        }

/// Creates a buffer from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkBuffer> VulkanDevice::create_buffer(const VkBufferCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_buffer");
            VkBuffer buf = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateBuffer(device_, &info, nullptr, &buf); result != VK_SUCCESS)
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    vulkan_failure_message("vkCreateBuffer", result, std::format("size {} bytes, usage 0x{:x}", info.size, info.usage)));
            return buf;
        }

/// Destroys the buffer identified by the supplied parameters.
///
/// @param buffer Buffer used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_buffer(VkBuffer buffer) noexcept { vkDestroyBuffer(device_, buffer, nullptr); }

/// Performs the buffer memory requirements operation for `Vulkan` using the supplied arguments.
///
/// @param buffer Buffer used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements VulkanDevice::buffer_memory_requirements(VkBuffer buffer) const noexcept {
            ZoneScopedN("VulkanDevice::buffer_memory_requirements");
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, buffer, &req);
            return req;
        }

/// Performs the buffer memory requirements2 operation for `Vulkan` using the supplied arguments.
///
/// @param buffer Buffer used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements2 VulkanDevice::buffer_memory_requirements2(VkBuffer buffer) const noexcept {
            ZoneScopedN("VulkanDevice::buffer_memory_requirements2");
            VkBufferMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .buffer = buffer,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetBufferMemoryRequirements2(device_, &query, &req);
            return req;
        }

/// Binds buffer memory for subsequent operations.
///
/// @param buffer Buffer used or affected by the operation.
/// @param memory `memory` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::bind_buffer_memory(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) noexcept {
            ZoneScopedN("VulkanDevice::bind_buffer_memory");
            if (const VkResult result = vkBindBufferMemory(device_, buffer, memory, offset); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkBindBufferMemory", result));
            return {};
        }

/// Performs the buffer device address operation for `Vulkan` using the supplied arguments.
///
/// @param buffer Buffer used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDeviceAddress VulkanDevice::buffer_device_address(VkBuffer buffer) const noexcept {
            ZoneScopedN("VulkanDevice::buffer_device_address");
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer,
            };
            return vkGetBufferDeviceAddress(device_, &info);
        }

/// Performs the buffer opaque capture address operation for `Vulkan` using the supplied arguments.
///
/// @param buffer Buffer used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] u64 VulkanDevice::buffer_opaque_capture_address(VkBuffer buffer) const noexcept {
            ZoneScopedN("VulkanDevice::buffer_opaque_capture_address");
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer,
            };
            return vkGetBufferOpaqueCaptureAddress(device_, &info);
        }

/// Creates a image from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkImage> VulkanDevice::create_image(const VkImageCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_image");
            VkImage img = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateImage(device_, &info, nullptr, &img); result != VK_SUCCESS)
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    vulkan_failure_message("vkCreateImage", result,
                                           std::format("{}x{}x{}, {} mips, {} layers, format {}, usage 0x{:x}", info.extent.width,
                                                       info.extent.height, info.extent.depth, info.mipLevels, info.arrayLayers,
                                                       static_cast<int>(info.format), info.usage)));
            return img;
        }

/// Destroys the image identified by the supplied parameters.
///
/// @param image `image` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_image(VkImage image) noexcept { vkDestroyImage(device_, image, nullptr); }

/// Performs the image memory requirements operation for `Vulkan` using the supplied arguments.
///
/// @param image `image` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements VulkanDevice::image_memory_requirements(VkImage image) const noexcept {
            ZoneScopedN("VulkanDevice::image_memory_requirements");
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device_, image, &req);
            return req;
        }

/// Performs the image memory requirements2 operation for `Vulkan` using the supplied arguments.
///
/// @param image `image` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements2 VulkanDevice::image_memory_requirements2(VkImage image) const noexcept {
            ZoneScopedN("VulkanDevice::image_memory_requirements2");
            VkImageMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .image = image,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetImageMemoryRequirements2(device_, &query, &req);
            return req;
        }

/// Binds image memory for subsequent operations.
///
/// @param image `image` value used by the operation.
/// @param memory `memory` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::bind_image_memory(VkImage image, VkDeviceMemory memory, VkDeviceSize offset) noexcept {
            ZoneScopedN("VulkanDevice::bind_image_memory");
            if (const VkResult result = vkBindImageMemory(device_, image, memory, offset); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkBindImageMemory", result));
            return {};
        }

/// Performs the image subresource layout operation for `Vulkan` using the supplied arguments.
///
/// @param image `image` value used by the operation.
/// @param subresource `subresource` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSubresourceLayout VulkanDevice::image_subresource_layout(VkImage image,
                                                                   const VkImageSubresource &subresource) const noexcept {
            ZoneScopedN("VulkanDevice::image_subresource_layout");
            VkSubresourceLayout layout{};
            vkGetImageSubresourceLayout(device_, image, &subresource, &layout);
            return layout;
        }

/// Creates a image view from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkImageView> VulkanDevice::create_image_view(const VkImageViewCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_image_view");
            VkImageView view = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateImageView(device_, &info, nullptr, &view); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateImageView", result));
            return view;
        }

/// Destroys the image view identified by the supplied parameters.
///
/// @param view `view` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_image_view(VkImageView view) noexcept { vkDestroyImageView(device_, view, nullptr); }

/// Creates a sampler from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkSampler> VulkanDevice::create_sampler(const VkSamplerCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_sampler");
            VkSampler sampler = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateSampler(device_, &info, nullptr, &sampler); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateSampler", result));
            return sampler;
        }

/// Destroys the sampler identified by the supplied parameters.
///
/// @param sampler Sampler used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_sampler(VkSampler sampler) noexcept { vkDestroySampler(device_, sampler, nullptr); }

/// Creates a shader module from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkShaderModule> VulkanDevice::create_shader_module(const VkShaderModuleCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_shader_module");
            VkShaderModule mod = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateShaderModule(device_, &info, nullptr, &mod); result != VK_SUCCESS)
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    vulkan_failure_message("vkCreateShaderModule", result, std::format("{} bytes of SPIR-V", info.codeSize)));
            return mod;
        }

/// Destroys the shader module identified by the supplied parameters.
///
/// @param shader_module Shader used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_shader_module(VkShaderModule shader_module) noexcept {
            ZoneScopedN("VulkanDevice::destroy_shader_module");
            vkDestroyShaderModule(device_, shader_module, nullptr);
        }

/// Creates a pipeline layout from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkPipelineLayout> VulkanDevice::create_pipeline_layout(const VkPipelineLayoutCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_pipeline_layout");
            VkPipelineLayout layout = VK_NULL_HANDLE;
            if (const VkResult result = vkCreatePipelineLayout(device_, &info, nullptr, &layout); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreatePipelineLayout", result));
            return layout;
        }

/// Destroys the pipeline layout identified by the supplied parameters.
///
/// @param layout `layout` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_pipeline_layout(VkPipelineLayout layout) noexcept {
            ZoneScopedN("VulkanDevice::destroy_pipeline_layout");
            vkDestroyPipelineLayout(device_, layout, nullptr);
        }

/// Creates a pipeline cache from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkPipelineCache> VulkanDevice::create_pipeline_cache(const VkPipelineCacheCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_pipeline_cache");
            VkPipelineCache cache = VK_NULL_HANDLE;
            if (const VkResult result = vkCreatePipelineCache(device_, &info, nullptr, &cache); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreatePipelineCache", result));
            return cache;
        }

/// Destroys the pipeline cache identified by the supplied parameters.
///
/// @param cache `cache` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_pipeline_cache(VkPipelineCache cache) noexcept {
            ZoneScopedN("VulkanDevice::destroy_pipeline_cache");
            vkDestroyPipelineCache(device_, cache, nullptr);
        }

/// Performs the merge pipeline caches operation for `Vulkan` using the supplied arguments.
///
/// @param dst Destination value or resource.
/// @param srcs `srcs` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::merge_pipeline_caches(VkPipelineCache dst,
                                                           span<const VkPipelineCache> srcs) noexcept {
            ZoneScopedN("VulkanDevice::merge_pipeline_caches");
            if (const VkResult result = vkMergePipelineCaches(device_, dst, static_cast<u32>(srcs.size()), srcs.data()); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkMergePipelineCaches", result));
            return {};
        }

/// Returns the pipeline cache data associated with this `Vulkan`.
///
/// @param cache `cache` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<u8>> VulkanDevice::pipeline_cache_data(VkPipelineCache cache) const {
            ZoneScopedN("VulkanDevice::pipeline_cache_data");
            usize size = 0;
            if (const VkResult sized = vkGetPipelineCacheData(device_, cache, &size, nullptr); sized != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetPipelineCacheData (size)", sized));
            vector<u8> data(size);
            if (const VkResult read = vkGetPipelineCacheData(device_, cache, &size, data.data()); read != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetPipelineCacheData (read)", read));
            return data;
        }

/// Creates a graphics pipeline from the supplied parameters.
///
/// @param cache `cache` value used by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkPipeline> VulkanDevice::create_graphics_pipeline(
            VkPipelineCache cache,
            const VkGraphicsPipelineCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_graphics_pipeline");
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateGraphicsPipelines(device_, cache, 1, &info, nullptr, &pipeline); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateGraphicsPipelines", result));
            return pipeline;
        }

/// Creates a graphics pipelines from the supplied parameters.
///
/// @param cache `cache` value used by the operation.
/// @param infos Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<VkPipeline>> VulkanDevice::create_graphics_pipelines(
            VkPipelineCache cache,
            span<const VkGraphicsPipelineCreateInfo> infos) {
            ZoneScopedN("VulkanDevice::create_graphics_pipelines");
            vector<VkPipeline> pipelines(infos.size(), VK_NULL_HANDLE);
            if (const VkResult result = vkCreateGraphicsPipelines(device_, cache, static_cast<u32>(infos.size()), infos.data(), nullptr, pipelines.data());
                result != VK_SUCCESS)
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    vulkan_failure_message("vkCreateGraphicsPipelines (batch)", result, std::format("{} pipelines requested", infos.size())));
            return pipelines;
        }

/// Creates a compute pipeline from the supplied parameters.
///
/// @param cache `cache` value used by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkPipeline> VulkanDevice::create_compute_pipeline(
            VkPipelineCache cache,
            const VkComputePipelineCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_compute_pipeline");
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateComputePipelines(device_, cache, 1, &info, nullptr, &pipeline); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateComputePipelines", result));
            return pipeline;
        }

/// Creates a compute pipelines from the supplied parameters.
///
/// @param cache `cache` value used by the operation.
/// @param infos Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<VkPipeline>> VulkanDevice::create_compute_pipelines(
            VkPipelineCache cache,
            span<const VkComputePipelineCreateInfo> infos) {
            ZoneScopedN("VulkanDevice::create_compute_pipelines");
            vector<VkPipeline> pipelines(infos.size(), VK_NULL_HANDLE);
            if (const VkResult result = vkCreateComputePipelines(device_, cache, static_cast<u32>(infos.size()), infos.data(), nullptr, pipelines.data());
                result != VK_SUCCESS)
                return graphics_backend_error(
                    GraphicsBackendErrorCode::OperationFailed,
                    vulkan_failure_message("vkCreateComputePipelines (batch)", result, std::format("{} pipelines requested", infos.size())));
            return pipelines;
        }

/// Destroys the pipeline identified by the supplied parameters.
///
/// @param pipeline Pipeline used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_pipeline(VkPipeline pipeline) noexcept { vkDestroyPipeline(device_, pipeline, nullptr); }

/// Creates a descriptor set layout from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkDescriptorSetLayout> VulkanDevice::create_descriptor_set_layout(
            const VkDescriptorSetLayoutCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_descriptor_set_layout");
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateDescriptorSetLayout(device_, &info, nullptr, &layout); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateDescriptorSetLayout", result));
            return layout;
        }

/// Destroys the descriptor set layout identified by the supplied parameters.
///
/// @param layout `layout` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_descriptor_set_layout(VkDescriptorSetLayout layout) noexcept {
            ZoneScopedN("VulkanDevice::destroy_descriptor_set_layout");
            vkDestroyDescriptorSetLayout(device_, layout, nullptr);
        }

/// Performs the descriptor set layout support operation for `Vulkan` using the supplied arguments.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDescriptorSetLayoutSupport VulkanDevice::descriptor_set_layout_support(
            const VkDescriptorSetLayoutCreateInfo &info) const noexcept {
            ZoneScopedN("VulkanDevice::descriptor_set_layout_support");
            VkDescriptorSetLayoutSupport support{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,
                .pNext = nullptr};
            vkGetDescriptorSetLayoutSupport(device_, &info, &support);
            return support;
        }

/// Creates a descriptor pool from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkDescriptorPool> VulkanDevice::create_descriptor_pool(
            const VkDescriptorPoolCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_descriptor_pool");
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateDescriptorPool(device_, &info, nullptr, &pool); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateDescriptorPool", result));
            return pool;
        }

/// Destroys the descriptor pool identified by the supplied parameters.
///
/// @param pool `pool` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_descriptor_pool(VkDescriptorPool pool) noexcept {
            ZoneScopedN("VulkanDevice::destroy_descriptor_pool");
            vkDestroyDescriptorPool(device_, pool, nullptr);
        }

/// Resets descriptor pool to its baseline state.
///
/// @param pool `pool` value used by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::reset_descriptor_pool(VkDescriptorPool pool,
                                                           VkDescriptorPoolResetFlags flags) noexcept {
            ZoneScopedN("VulkanDevice::reset_descriptor_pool");
            if (const VkResult result = vkResetDescriptorPool(device_, pool, flags); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkResetDescriptorPool", result));
            return {};
        }

/// Allocates descriptor sets.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
[[nodiscard]] RendererExpected<vector<VkDescriptorSet>> VulkanDevice::allocate_descriptor_sets(
            const VkDescriptorSetAllocateInfo &info) {
            ZoneScopedN("VulkanDevice::allocate_descriptor_sets");
            vector<VkDescriptorSet> sets(info.descriptorSetCount, VK_NULL_HANDLE);
            if (const VkResult result = vkAllocateDescriptorSets(device_, &info, sets.data()); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory,
                                              vulkan_failure_message("vkAllocateDescriptorSets", result,
                                                                     std::format("{} sets requested", info.descriptorSetCount)));
            return sets;
        }

/// Releases previously allocated storage or resources.
///
/// @param pool `pool` value used by the operation.
/// @param sets `sets` value used by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::free_descriptor_sets(VkDescriptorPool pool,
                                                          span<const VkDescriptorSet> sets) noexcept {
            ZoneScopedN("VulkanDevice::free_descriptor_sets");
            if (const VkResult result = vkFreeDescriptorSets(device_, pool, static_cast<u32>(sets.size()), sets.data()); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkFreeDescriptorSets", result));
            return {};
        }

/// Updates descriptor sets from the supplied values.
///
/// @param writes `writes` value used by the operation.
/// @param copies `copies` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::update_descriptor_sets(span<const VkWriteDescriptorSet> writes,
                                    span<const VkCopyDescriptorSet> copies) noexcept {
            ZoneScopedN("VulkanDevice::update_descriptor_sets");
            vkUpdateDescriptorSets(device_,
                                   static_cast<u32>(writes.size()),
                                   writes.data(),
                                   static_cast<u32>(copies.size()),
                                   copies.data());
        }

/// Creates a command pool from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkCommandPool> VulkanDevice::create_command_pool(
            const VkCommandPoolCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_command_pool");
            VkCommandPool pool = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateCommandPool(device_, &info, nullptr, &pool); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateCommandPool", result));
            return pool;
        }

/// Destroys the command pool identified by the supplied parameters.
///
/// @param pool `pool` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_command_pool(VkCommandPool pool) noexcept {
            ZoneScopedN("VulkanDevice::destroy_command_pool");
            vkDestroyCommandPool(device_, pool, nullptr);
        }

/// Resets command pool to its baseline state.
///
/// @param pool `pool` value used by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::reset_command_pool(VkCommandPool pool,
                                                        VkCommandPoolResetFlags flags) noexcept {
            ZoneScopedN("VulkanDevice::reset_command_pool");
            if (const VkResult result = vkResetCommandPool(device_, pool, flags); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkResetCommandPool", result));
            return {};
        }

/// Performs the trim command pool operation for `Vulkan` using the supplied arguments.
///
/// @param pool `pool` value used by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::trim_command_pool(VkCommandPool pool, VkCommandPoolTrimFlags flags) noexcept {
            ZoneScopedN("VulkanDevice::trim_command_pool");
            vkTrimCommandPool(device_, pool, flags);
        }

/// Allocates command buffers.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OutOfMemory`.
[[nodiscard]] RendererExpected<vector<VkCommandBuffer>> VulkanDevice::allocate_command_buffers(
            const VkCommandBufferAllocateInfo &info) {
            ZoneScopedN("VulkanDevice::allocate_command_buffers");
            vector<VkCommandBuffer> buffers(info.commandBufferCount, VK_NULL_HANDLE);
            if (const VkResult result = vkAllocateCommandBuffers(device_, &info, buffers.data()); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OutOfMemory, vulkan_failure_message("vkAllocateCommandBuffers", result));
            return buffers;
        }

/// Releases previously allocated storage or resources.
///
/// @param pool `pool` value used by the operation.
/// @param buffers Buffer used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::free_command_buffers(VkCommandPool pool, span<const VkCommandBuffer> buffers) noexcept {
            ZoneScopedN("VulkanDevice::free_command_buffers");
            vkFreeCommandBuffers(device_, pool, static_cast<u32>(buffers.size()), buffers.data());
        }

/// Resets command buffer to its baseline state.
///
/// @param buffer Buffer used or affected by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::reset_command_buffer(VkCommandBuffer buffer,
                                                          VkCommandBufferResetFlags flags) noexcept {
            ZoneScopedN("VulkanDevice::reset_command_buffer");
            if (const VkResult result = vkResetCommandBuffer(buffer, flags); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkResetCommandBuffer", result));
            return {};
        }

/// Creates a fence from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkFence> VulkanDevice::create_fence(const VkFenceCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_fence");
            VkFence fence = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateFence(device_, &info, nullptr, &fence); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateFence", result));
            return fence;
        }

/// Destroys the fence identified by the supplied parameters.
///
/// @param fence Fence used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_fence(VkFence fence) noexcept { vkDestroyFence(device_, fence, nullptr); }

/// Resets fences to its baseline state.
///
/// @param fences Fence used or affected by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::reset_fences(span<const VkFence> fences) noexcept {
            ZoneScopedN("VulkanDevice::reset_fences");
            if (const VkResult result = vkResetFences(device_, static_cast<u32>(fences.size()), fences.data()); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkResetFences", result));
            return {};
        }

/// Reports whether fence signaled holds for this `Vulkan`.
///
/// @param fence Fence used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<bool> VulkanDevice::is_fence_signaled(VkFence fence) const noexcept {
            ZoneScopedN("VulkanDevice::is_fence_signaled");
            VkResult res = vkGetFenceStatus(device_, fence);
            if (res == VK_SUCCESS)
                return true;
            if (res == VK_NOT_READY)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetFenceStatus", res));
        }

/// Creates a semaphore from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkSemaphore> VulkanDevice::create_semaphore(const VkSemaphoreCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_semaphore");
            VkSemaphore semaphore = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateSemaphore(device_, &info, nullptr, &semaphore); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateSemaphore", result));
            return semaphore;
        }

/// Destroys the semaphore identified by the supplied parameters.
///
/// @param semaphore Semaphore used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_semaphore(VkSemaphore semaphore) noexcept {
            ZoneScopedN("VulkanDevice::destroy_semaphore");
            vkDestroySemaphore(device_, semaphore, nullptr);
        }

/// Performs the semaphore counter value operation for `Vulkan` using the supplied arguments.
///
/// @param semaphore Semaphore used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<u64> VulkanDevice::semaphore_counter_value(VkSemaphore semaphore) const noexcept {
            ZoneScopedN("VulkanDevice::semaphore_counter_value");
            u64 value = 0;
            if (const VkResult result = vkGetSemaphoreCounterValue(device_, semaphore, &value); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetSemaphoreCounterValue", result));
            return value;
        }

/// Signals semaphore.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::signal_semaphore(const VkSemaphoreSignalInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::signal_semaphore");
            if (const VkResult result = vkSignalSemaphore(device_, &info); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkSignalSemaphore", result));
            return {};
        }

/// Waits for semaphores to complete.
///
/// @param info Description of the resource or operation to perform.
/// @param timeout_ns Maximum amount of time to wait before giving up.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::wait_semaphores(const VkSemaphoreWaitInfo &info, u64 timeout_ns) noexcept {
            ZoneScopedN("VulkanDevice::wait_semaphores");
            VkResult res = vkWaitSemaphores(device_, &info, timeout_ns);
            if (res != VK_SUCCESS && res != VK_TIMEOUT)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkWaitSemaphores", res));
            return {};
        }

/// Creates a event from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkEvent> VulkanDevice::create_event(const VkEventCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_event");
            VkEvent event = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateEvent(device_, &info, nullptr, &event); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateEvent", result));
            return event;
        }

/// Destroys the event identified by the supplied parameters.
///
/// @param event Event used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_event(VkEvent event) noexcept { vkDestroyEvent(device_, event, nullptr); }

/// Performs the event status operation for `Vulkan` using the supplied arguments.
///
/// @param event Event used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<bool> VulkanDevice::event_status(VkEvent event) const noexcept {
            ZoneScopedN("VulkanDevice::event_status");
            VkResult res = vkGetEventStatus(device_, event);
            if (res == VK_EVENT_SET)
                return true;
            if (res == VK_EVENT_RESET)
                return false;
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetEventStatus", res));
        }

/// Sets the event for this `Vulkan`.
///
/// @param event Event used or affected by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::set_event(VkEvent event) noexcept {
            ZoneScopedN("VulkanDevice::set_event");
            if (const VkResult result = vkSetEvent(device_, event); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkSetEvent", result));
            return {};
        }

/// Resets event to its baseline state.
///
/// @param event Event used or affected by the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::reset_event(VkEvent event) noexcept {
            ZoneScopedN("VulkanDevice::reset_event");
            if (const VkResult result = vkResetEvent(device_, event); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkResetEvent", result));
            return {};
        }

/// Creates a query pool from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkQueryPool> VulkanDevice::create_query_pool(const VkQueryPoolCreateInfo &info) noexcept {
            ZoneScopedN("VulkanDevice::create_query_pool");
            VkQueryPool pool = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateQueryPool(device_, &info, nullptr, &pool); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateQueryPool", result));
            return pool;
        }

/// Destroys the query pool identified by the supplied parameters.
///
/// @param pool `pool` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_query_pool(VkQueryPool pool) noexcept { vkDestroyQueryPool(device_, pool, nullptr); }

/// Returns the query pool results associated with this `Vulkan`.
///
/// @param pool `pool` value used by the operation.
/// @param first_query `first_query` value used by the operation.
/// @param query_count Number of elements or operations to process.
/// @param data Data consumed or referenced by the operation.
/// @param stride `stride` value used by the operation.
/// @param flags Flags controlling optional behavior.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanDevice::get_query_pool_results(VkQueryPool pool, u32 first_query, u32 query_count, span<u8> data, VkDeviceSize stride, VkQueryResultFlags flags) noexcept {
            ZoneScopedN("VulkanDevice::get_query_pool_results");
            VkResult res = vkGetQueryPoolResults(device_, pool, first_query, query_count, data.size_bytes(), data.data(), stride, flags);
            if (res != VK_SUCCESS && res != VK_NOT_READY)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetQueryPoolResults", res));
            return {};
        }

/// Resets query pool to its baseline state.
///
/// @param pool `pool` value used by the operation.
/// @param first_query `first_query` value used by the operation.
/// @param query_count Number of elements or operations to process.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::reset_query_pool(VkQueryPool pool, u32 first_query, u32 query_count) noexcept {
            ZoneScopedN("VulkanDevice::reset_query_pool");
            vkResetQueryPool(device_, pool, first_query, query_count);
        }

/// Creates a swapchain from the supplied parameters.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VkSwapchainKHR> VulkanDevice::create_swapchain(
            const VkSwapchainCreateInfoKHR &info) noexcept {
            ZoneScopedN("VulkanDevice::create_swapchain");
            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            if (const VkResult result = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain); result != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkCreateSwapchainKHR", result));
            return swapchain;
        }

/// Destroys the swapchain identified by the supplied parameters.
///
/// @param swapchain Swapchain used or affected by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::destroy_swapchain(VkSwapchainKHR swapchain) noexcept {
            ZoneScopedN("VulkanDevice::destroy_swapchain");
            vkDestroySwapchainKHR(device_, swapchain, nullptr);
        }

/// Performs the swapchain images operation for `Vulkan` using the supplied arguments.
///
/// @param swapchain Swapchain used or affected by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
[[nodiscard]] RendererExpected<vector<VkImage>> VulkanDevice::swapchain_images(VkSwapchainKHR swapchain) const {
            ZoneScopedN("VulkanDevice::swapchain_images");
            u32 count = 0;
            if (const VkResult counted = vkGetSwapchainImagesKHR(device_, swapchain, &count, nullptr); counted != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetSwapchainImagesKHR (count)", counted));
            vector<VkImage> images(count, VK_NULL_HANDLE);
            if (const VkResult populated = vkGetSwapchainImagesKHR(device_, swapchain, &count, images.data()); populated != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkGetSwapchainImagesKHR (populate)", populated));
            return images;
        }

/// Acquires next image.
///
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<u32> VulkanDevice::acquire_next_image(const VkAcquireNextImageInfoKHR &info) noexcept {
            ZoneScopedN("VulkanDevice::acquire_next_image");
            u32 index = 0;
            VkResult res = vkAcquireNextImage2KHR(device_, &info, &index);
            if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, vulkan_failure_message("vkAcquireNextImage2KHR", res));
            return index;
        }

/// Returns a human-readable name for the supplied set debug value.
///
/// @param type Type value to inspect, select, or convert.
/// @param object_handle Handle identifying the target object or resource.
/// @param name Name used to identify or label the target.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::set_debug_name(VkObjectType type, u64 object_handle, const char *name) noexcept {
            ZoneScopedN("VulkanDevice::set_debug_name");
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

/// Sets the debug tag for this `Vulkan`.
///
/// @param type Type value to inspect, select, or convert.
/// @param object_handle Handle identifying the target object or resource.
/// @param tag_name Name used to identify or label the target.
/// @param tag_data Data consumed or referenced by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
void VulkanDevice::set_debug_tag(VkObjectType type, u64 object_handle, u64 tag_name, span<const u8> tag_data) noexcept {
            ZoneScopedN("VulkanDevice::set_debug_tag");
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
