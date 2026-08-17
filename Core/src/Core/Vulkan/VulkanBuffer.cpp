#include "VulkanBuffer.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanBuffer::~VulkanBuffer() { destroy(); }

/// Performs the vulkan buffer operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanBuffer::VulkanBuffer(VulkanBuffer &&o) noexcept
            : device_(o.device_), allocator_(o.allocator_), buffer_(o.buffer_), allocation_(o.allocation_),
              size_(o.size_), usage_(o.usage_) {
            ZoneScopedN("VulkanBuffer::VulkanBuffer");
            o.device_ = VK_NULL_HANDLE;
            o.allocator_ = VK_NULL_HANDLE;
            o.buffer_ = VK_NULL_HANDLE;
            o.allocation_ = VK_NULL_HANDLE;
            o.size_ = 0;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanBuffer &VulkanBuffer::operator=(VulkanBuffer &&o) noexcept {
            ZoneScopedN("VulkanBuffer::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param size Requested or available size for the operation.
/// @param usage Usage flags or category applied to the resource.
/// @param flags Flags controlling optional behavior.
/// @param sharing `sharing` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanBuffer> VulkanBuffer::create(
            VkDevice device,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkBufferCreateFlags flags,
            VkSharingMode sharing) noexcept {
            ZoneScopedN("VulkanBuffer::create");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param allocator Allocator used for storage owned by the operation.
/// @param buffer_info Description of the resource or operation to perform.
/// @param allocation_info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanBuffer> VulkanBuffer::create(
            VkDevice device,
            VmaAllocator allocator,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept {
            ZoneScopedN("VulkanBuffer::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkBuffer VulkanBuffer::vk_handle() const noexcept { return buffer_; }

/// Returns the current or globally available allocation value.
///
/// @return Returns the current allocation value.
/// @note This function does not throw exceptions.
[[nodiscard]] VmaAllocation VulkanBuffer::allocation() const noexcept { return allocation_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanBuffer::is_valid() const noexcept { return buffer_ != VK_NULL_HANDLE; }

/// Returns the current or globally available owns allocation value.
///
/// @return Returns the current owns allocation value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanBuffer::owns_allocation() const noexcept { return allocation_ != VK_NULL_HANDLE; }

/// Returns the size for this `Vulkan`.
///
/// @return Returns the current size value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDeviceSize VulkanBuffer::size() const noexcept { return size_; }

/// Returns the current or globally available usage value.
///
/// @return Returns the current usage value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkBufferUsageFlags VulkanBuffer::usage() const noexcept { return usage_; }

/// Uploads the supplied or associated value/state using the supplied arguments and current state.
///
/// @param data Data consumed or referenced by the operation.
/// @param bytes Size of the relevant data in bytes.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanBuffer::upload(const void *data, VkDeviceSize bytes, VkDeviceSize offset) noexcept {
            ZoneScopedN("VulkanBuffer::upload");
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            std::memcpy(static_cast<std::byte *>(mapped) + offset, data, bytes);


            if (vmaFlushAllocation(allocator_, allocation_, offset, bytes) != VK_SUCCESS) {
                vmaUnmapMemory(allocator_, allocation_);
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaFlushAllocation failed.");
            }
            vmaUnmapMemory(allocator_, allocation_);
            return {};
        }

/// Performs the download operation for `Vulkan` using the supplied arguments.
///
/// @param dst Destination value or resource.
/// @param bytes Size of the relevant data in bytes.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanBuffer::download(void *dst, VkDeviceSize bytes, VkDeviceSize offset) noexcept {
            ZoneScopedN("VulkanBuffer::download");
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            std::memcpy(dst, static_cast<const std::byte *>(mapped) + offset, bytes);
            vmaUnmapMemory(allocator_, allocation_);
            return {};
        }

/// Maps the requested resource for access.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<void *> VulkanBuffer::map() noexcept {
            ZoneScopedN("VulkanBuffer::map");
            void *mapped = nullptr;
            if (vmaMapMemory(allocator_, allocation_, &mapped) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaMapMemory failed.");
            return mapped;
        }

/// Unmaps the previously mapped resource.
///
/// @return Returns the current unmap value.
/// @note This function does not throw exceptions.
void VulkanBuffer::unmap() noexcept {
            ZoneScopedN("VulkanBuffer::unmap");
            if (allocation_ != VK_NULL_HANDLE) {
                vmaUnmapMemory(allocator_, allocation_);
            }
        }

/// Flushes pending work or buffered state.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanBuffer::flush(VkDeviceSize offset, VkDeviceSize size) noexcept {
            ZoneScopedN("VulkanBuffer::flush");
            if (vmaFlushAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaFlushAllocation failed.");
            return {};
        }

/// Performs the invalidate operation for `Vulkan` using the supplied arguments.
///
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanBuffer::invalidate(VkDeviceSize offset, VkDeviceSize size) noexcept {
            ZoneScopedN("VulkanBuffer::invalidate");
            if (vmaInvalidateAllocation(allocator_, allocation_, offset, size) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vmaInvalidateAllocation failed.");
            return {};
        }

/// Returns the current or globally available memory requirements value.
///
/// @return Returns the current memory requirements value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements VulkanBuffer::memory_requirements() const noexcept {
            ZoneScopedN("VulkanBuffer::memory_requirements");
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device_, buffer_, &req);
            return req;
        }

/// Returns the current or globally available memory requirements2 value.
///
/// @return Returns the current memory requirements2 value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkMemoryRequirements2 VulkanBuffer::memory_requirements2() const noexcept {
            ZoneScopedN("VulkanBuffer::memory_requirements2");
            VkBufferMemoryRequirementsInfo2 query{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                .pNext = nullptr,
                .buffer = buffer_,
            };
            VkMemoryRequirements2 req{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = nullptr};
            vkGetBufferMemoryRequirements2(device_, &query, &req);
            return req;
        }

/// Binds memory for subsequent operations.
///
/// @param memory `memory` value used by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
///
/// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererResult VulkanBuffer::bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset) noexcept {
            ZoneScopedN("VulkanBuffer::bind_memory");
            if (vkBindBufferMemory(device_, buffer_, memory, offset) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkBindBufferMemory failed.");
            return {};
        }

/// Returns the current or globally available device address value.
///
/// @return Returns the current device address value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDeviceAddress VulkanBuffer::device_address() const noexcept {
            ZoneScopedN("VulkanBuffer::device_address");
            VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = buffer_,
            };
            return vkGetBufferDeviceAddress(device_, &info);
        }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanBuffer::destroy() noexcept {
            ZoneScopedN("VulkanBuffer::destroy");
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

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanBufferView::~VulkanBufferView() { destroy(); }

/// Performs the vulkan buffer view operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanBufferView::VulkanBufferView(VulkanBufferView &&o) noexcept
            : device_(o.device_), view_(o.view_), format_(o.format_) {
            ZoneScopedN("VulkanBufferView::VulkanBufferView");
            o.device_ = VK_NULL_HANDLE;
            o.view_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanBufferView &VulkanBufferView::operator=(VulkanBufferView &&o) noexcept {
            ZoneScopedN("VulkanBufferView::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param buffer Buffer used or affected by the operation.
/// @param format Format used for the resource, render target, or conversion.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param range Range of values to process.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanBufferView> VulkanBufferView::create(
            VkDevice device,
            VkBuffer buffer,
            VkFormat format,
            VkDeviceSize offset,
            VkDeviceSize range) noexcept {
            ZoneScopedN("VulkanBufferView::create");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkBufferView VulkanBufferView::vk_handle() const noexcept { return view_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanBufferView::is_valid() const noexcept { return view_ != VK_NULL_HANDLE; }

/// Formats the supplied value into the provided formatting context.
///
/// @return Returns the current format value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkFormat VulkanBufferView::format() const noexcept { return format_; }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanBufferView::destroy() noexcept {
            ZoneScopedN("VulkanBufferView::destroy");
            if (view_ == VK_NULL_HANDLE)
                return;
            vkDestroyBufferView(device_, view_, nullptr);
            view_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
