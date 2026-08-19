#pragma once

#include <Foundation/Foundation.hpp>
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
#include <cstddef>
#include <cstring>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using SFT::Core::RendererResult;

namespace SFT::Core::Vulkan {


    class VulkanBuffer {
      public:
        /// Constructs a `VulkanBuffer` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanBuffer() = default;
        /// Destroys the `VulkanBuffer` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanBuffer();

        /// Disables this construction form for `VulkanBuffer`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanBuffer(const VulkanBuffer &) = delete;
        /// Assigns a new value to this `VulkanBuffer`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanBuffer &operator=(const VulkanBuffer &) = delete;

        /// Constructs a `VulkanBuffer` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanBuffer(VulkanBuffer &&o) noexcept;
        /// Assigns a new value to this `VulkanBuffer`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanBuffer &operator=(VulkanBuffer &&o) noexcept;

        /// Creates a `VulkanBuffer` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param size Requested or available size for the operation.
        /// @param usage Usage flags or category applied to the resource.
        /// @param flags Flags controlling optional behavior.
        /// @param sharing `sharing` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanBuffer> create(
            VkDevice device,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VkBufferCreateFlags flags = 0,
            VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE) noexcept;

        /// Creates a `VulkanBuffer` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param allocator Allocator used for storage owned by the operation.
        /// @param buffer_info Description of the resource or operation to perform.
        /// @param allocation_info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanBuffer> create(
            VkDevice device,
            VmaAllocator allocator,
            const VkBufferCreateInfo &buffer_info,
            const VmaAllocationCreateInfo &allocation_info) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanBuffer`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkBuffer vk_handle() const noexcept;
        /// Returns the current or globally available allocation value.
        ///
        /// @return Returns the current allocation value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VmaAllocation allocation() const noexcept;
        /// Reports whether valid holds for this `VulkanBuffer`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Returns the current or globally available owns allocation value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool owns_allocation() const noexcept;
        /// Returns the size for this `VulkanBuffer`.
        ///
        /// @return Returns the current size value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDeviceSize size() const noexcept;
        /// Returns the current or globally available usage value.
        ///
        /// @return Returns the current usage value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkBufferUsageFlags usage() const noexcept;

        /// Reports whether this allocation actually landed in HOST_VISIBLE memory.
        ///
        /// @return true if the allocation's real memory type carries VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT.
        /// @note Meaningful for any allocation, but this is what makes opportunistic Resizable BAR
        ///       placement observable after the fact: a buffer created with MemoryLocation::DeviceLocal
        ///       (VulkanRhiConvert.hpp's to_vma()) asks VMA to prefer a combined DEVICE_LOCAL|
        ///       HOST_VISIBLE memory type when the GPU exposes one large enough, but VMA silently
        ///       falls back to plain device-local memory when it doesn't (or the allocation is too
        ///       large for that heap) — there is no way to know which happened except asking the
        ///       allocation itself. See VulkanRhiBridgeBuffers.cpp's create_buffer()/write_buffer().
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_host_visible() const noexcept;


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
        [[nodiscard]] RendererResult upload(const void *data, VkDeviceSize bytes, VkDeviceSize offset = 0) noexcept;


        /// Performs the download operation for `VulkanBuffer` using the supplied arguments.
        ///
        /// @param dst Destination value or resource.
        /// @param bytes Size of the relevant data in bytes.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult download(void *dst, VkDeviceSize bytes, VkDeviceSize offset = 0) noexcept;


        /// Maps the requested resource for access.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererExpected<void *> map() noexcept;
        /// Unmaps the previously mapped resource.
        ///
        /// @note This function does not throw exceptions.
        void unmap() noexcept;


        /// Flushes pending work or buffered state.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult flush(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept;
        /// Performs the invalidate operation for `VulkanBuffer` using the supplied arguments.
        ///
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult invalidate(VkDeviceSize offset = 0, VkDeviceSize size = VK_WHOLE_SIZE) noexcept;

        /// Returns the current or globally available memory requirements value.
        ///
        /// @return Returns the current memory requirements value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements memory_requirements() const noexcept;

        /// Returns the current or globally available memory requirements2 value.
        ///
        /// @return Returns the current memory requirements2 value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkMemoryRequirements2 memory_requirements2() const noexcept;

        /// Binds memory for subsequent operations.
        ///
        /// @param memory `memory` value used by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererResult bind_memory(VkDeviceMemory memory,
                                                 VkDeviceSize offset = 0) noexcept;


        /// Returns the current or globally available device address value.
        ///
        /// @return Returns the current device address value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDeviceAddress device_address() const noexcept;

        /// Destroys or releases the `VulkanBuffer` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VmaAllocator allocator_ = VK_NULL_HANDLE;
        VkBuffer buffer_ = VK_NULL_HANDLE;
        VmaAllocation allocation_ = VK_NULL_HANDLE;
        VkDeviceSize size_ = 0;
        VkBufferUsageFlags usage_ = 0;
    };


    class VulkanBufferView {
      public:
        /// Constructs a `VulkanBufferView` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanBufferView() = default;
        /// Destroys the `VulkanBufferView` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanBufferView();

        /// Disables this construction form for `VulkanBufferView`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanBufferView(const VulkanBufferView &) = delete;
        /// Assigns a new value to this `VulkanBufferView`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanBufferView &operator=(const VulkanBufferView &) = delete;

        /// Constructs a `VulkanBufferView` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanBufferView(VulkanBufferView &&o) noexcept;
        /// Assigns a new value to this `VulkanBufferView`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanBufferView &operator=(VulkanBufferView &&o) noexcept;

        /// Creates a `VulkanBufferView` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param buffer Buffer used or affected by the operation.
        /// @param format Format used for the resource, render target, or conversion.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param range Range of values to process.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanBufferView> create(
            VkDevice device,
            VkBuffer buffer,
            VkFormat format,
            VkDeviceSize offset = 0,
            VkDeviceSize range = VK_WHOLE_SIZE) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanBufferView`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkBufferView vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanBufferView`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Formats the supplied value into the provided formatting context.
        ///
        /// @return Returns the current format value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkFormat format() const noexcept;

        /// Destroys or releases the `VulkanBufferView` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkBufferView view_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
    };

} // namespace SFT::Core::Vulkan
