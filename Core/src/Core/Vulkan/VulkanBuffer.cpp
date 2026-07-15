#include "VulkanBuffer.hpp"

namespace SFT::Core::Vulkan {

VulkanBuffer::~VulkanBuffer() { destroy(); }

VulkanBuffer::VulkanBuffer(VulkanBuffer &&o) noexcept
            : device_(o.device_), allocator_(o.allocator_), buffer_(o.buffer_), allocation_(o.allocation_),
              size_(o.size_), usage_(o.usage_) {
            o.device_ = VK_NULL_HANDLE;
            o.allocator_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.allocation_ = VK_NULL_HANDLE;
            o.size_ = 0;
        }

VulkanBuffer &VulkanBuffer::operator=(VulkanBuffer &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                allocator_ = o.allocator_;
                buffer_ = o.buffer_;
                allocation_ = o.allocation_;
                size_ = o.size_;
                usage_ = o.usage_;
                o.device_ = VK_NULL_HANDLE;
                o.allocator_ = VK_NULL_HANDLE;
                o.buffer_ = VK_NULL_HANDLE;
                o.allocation_ = VK_NULL_HANDLE;
                o.size_ = 0;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanBuffer> VulkanBuffer::create(
            VkDevice device,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkBufferCreateFlags flags,
            VkSharingMode sharing) noexcept {
            VkBufferCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .size = size,
                .usage = usage,
                .sharingMode = sharing,
            };
            VkBuffer buf = VK_NULL_HANDLE;
            if (vkCreateBuffer(device, &info, nullptr, &buf) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateBuffer failed.");
            VulkanBuffer out;
            out.device_ = device;
            out.buffer_ = buf;
            out.size_ = size;
            out.usage_ = usage;
            return out;
        }

[[nodiscard]] RendererExpected<VulkanBuffer> VulkanBuffer::create(
            VkDevice device,
            VmaAllocator allocator,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept {
            VkBuffer buf = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            if (vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &buf, &allocation, nullptr) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaCreateBuffer failed.");
            VulkanBuffer out;
            out.device_ = device;
            out.allocator_ = allocator;
            out.buffer_ = buf;
            out.allocation_ = allocation;
            out.size_ = buffer_info.size;
            out.usage_ = buffer_info.usage;
            return out;
        }

[[nodiscard]] VkBuffer VulkanBuffer::vk_handle() const noexcept { return buffer_; }

[[nodiscard]] VmaAllocation VulkanBuffer::allocation() const noexcept { return allocation_; }

[[nodiscard]] bool VulkanBuffer::is_valid() const noexcept { return buffer_ != VK_NULL_HANDLE; }

[[nodiscard]] bool VulkanBuffer::owns_allocation() const noexcept { return allocation_ != VK_NULL_HANDLE; }

[[nodiscard]] VkDeviceSize VulkanBuffer::size() const noexcept { return size_; }

[[nodiscard]] VkBufferUsageFlags VulkanBuffer::usage() const noexcept { return usage_; }

[[nodiscard]] RendererResult VulkanBuffer::upload(const void *data, VkDeviceSize bytes, VkDeviceSize offset) noexcept {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            std::memcpy(static_cast<std::byte *>(mapped) + offset, data, bytes);
            // HOST_ACCESS_SEQUENTIAL_WRITE only asks VMA to *prefer* a coherent memory type - it
            // is not guaranteed (observed non-coherent on RADV), so the write is not guaranteed
            // visible to the GPU until flushed. vmaFlushAllocation is a no-op on truly coherent
            // memory, so this is always safe to call.
            if (vmaFlushAllocation(allocator_, allocation_, offset, bytes) != VK_SUCCESS) {
                vmaUnmapMemory(allocator_, allocation_);
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaFlushAllocation failed.");
            }
            vmaUnmapMemory(allocator_, allocation_);
            return {};
        }

[[nodiscard]] RendererResult VulkanBuffer::download(void *dst, VkDeviceSize bytes, VkDeviceSize offset) noexcept {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            std::memcpy(dst, static_cast<const std::byte *>(mapped) + offset, bytes);
            vmaUnmapMemory(allocator_, allocation_);
            return {};
        }

[[nodiscard]] RendererExpected<void *> VulkanBuffer::map() noexcept {
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            return mapped;
        }

void VulkanBuffer::unmap() noexcept {
            if (allocation_ != VK_NULL_HANDLE) {
                vmaUnmapMemory(allocator_, allocation_);
            }
        }

[[nodiscard]] RendererResult VulkanBuffer::flush(VkDeviceSize offset, VkDeviceSize size) noexcept {
            if (vmaFlushAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaFlushAllocation failed.");
            return {};
        }

[[nodiscard]] RendererResult VulkanBuffer::invalidate(VkDeviceSize offset, VkDeviceSize size) noexcept {
            if (vmaInvalidateAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaInvalidateAllocation failed.");
            return {};
        }

[[nodiscard]] VkMemoryRequirements VulkanBuffer::memory_requirements() const noexcept {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, buffer_, &req);
            return req;
        }

[[nodiscard]] VkMemoryRequirements2 VulkanBuffer::memory_requirements2() const noexcept {
            VkBufferMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .buffer = buffer_,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetBufferMemoryRequirements2(device_, &query, &req);
            return req;
        }

[[nodiscard]] RendererResult VulkanBuffer::bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset) noexcept {
            if (vkBindBufferMemory(device_, buffer_, memory, offset) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBindBufferMemory failed.");
            return {};
        }

[[nodiscard]] VkDeviceAddress VulkanBuffer::device_address() const noexcept {
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer_,
            };
            return vkGetBufferDeviceAddress(device_, &info);
        }

void VulkanBuffer::destroy() noexcept {
            if (buffer_ == VK_NULL_HANDLE)
                return;

            if (allocation_ != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator_, buffer_, allocation_);
                allocation_ = VK_NULL_HANDLE;
                allocator_ = VK_NULL_HANDLE;
            } else {
                vkDestroyBuffer(device_, buffer_, nullptr);
            }

            buffer_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
            size_ = 0;
            usage_ = 0;
        }

VulkanBufferView::~VulkanBufferView() { destroy(); }

VulkanBufferView::VulkanBufferView(VulkanBufferView &&o) noexcept
            : device_(o.device_), view_(o.view_), format_(o.format_) {
            o.device_ = VK_NULL_HANDLE;
            o.view_ = VK_NULL_HANDLE;
        }

VulkanBufferView &VulkanBufferView::operator=(VulkanBufferView &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                view_ = o.view_;
                format_ = o.format_;
                o.device_ = VK_NULL_HANDLE;
                o.view_ = VK_NULL_HANDLE;
            }
            return *this;
        }

[[nodiscard]] RendererExpected<VulkanBufferView> VulkanBufferView::create(
            VkDevice device,
            VkBuffer buffer,
            VkFormat format,
            VkDeviceSize offset,
            VkDeviceSize range) noexcept {
            VkBufferViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .buffer = buffer,
                .format = format,
                .offset = offset,
                .range = range,
            };
            VkBufferView view = VK_NULL_HANDLE;
            if (vkCreateBufferView(device, &info, nullptr, &view) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateBufferView failed.");
            VulkanBufferView out;
            out.device_ = device;
            out.view_ = view;
            out.format_ = format;
            return out;
        }

[[nodiscard]] VkBufferView VulkanBufferView::vk_handle() const noexcept { return view_; }

[[nodiscard]] bool VulkanBufferView::is_valid() const noexcept { return view_ != VK_NULL_HANDLE; }

[[nodiscard]] VkFormat VulkanBufferView::format() const noexcept { return format_; }

void VulkanBufferView::destroy() noexcept {
            if (view_ == VK_NULL_HANDLE)
                return;
            vkDestroyBufferView(device_, view_, nullptr);
            view_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
