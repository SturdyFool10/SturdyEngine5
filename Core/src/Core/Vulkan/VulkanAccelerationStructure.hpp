#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <span>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;
using std::span;

namespace SFT::Core::Vulkan {


    class VulkanAccelerationStructure {
      public:
        /// Constructs a `VulkanAccelerationStructure` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanAccelerationStructure() = default;
        /// Destroys the `VulkanAccelerationStructure` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanAccelerationStructure();

        /// Disables this construction form for `VulkanAccelerationStructure`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanAccelerationStructure(const VulkanAccelerationStructure &) = delete;
        /// Assigns a new value to this `VulkanAccelerationStructure`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanAccelerationStructure &operator=(const VulkanAccelerationStructure &) = delete;

        /// Constructs a `VulkanAccelerationStructure` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanAccelerationStructure(VulkanAccelerationStructure &&o) noexcept;
        /// Assigns a new value to this `VulkanAccelerationStructure`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanAccelerationStructure &operator=(VulkanAccelerationStructure &&o) noexcept;


        /// Creates a `VulkanAccelerationStructure` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param backing_buffer Buffer used or affected by the operation.
        /// @param offset Offset from the beginning of the relevant range or buffer.
        /// @param size Requested or available size for the operation.
        /// @param type Type value to inspect, select, or convert.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanAccelerationStructure> create(
            VkDevice device,
            VkBuffer backing_buffer,
            VkDeviceSize offset,
            VkDeviceSize size,
            VkAccelerationStructureTypeKHR type) noexcept;


        /// Builds sizes.
        ///
        /// @param device Device used or affected by the operation.
        /// @param build_info Description of the resource or operation to perform.
        /// @param max_primitive_counts `max_primitive_counts` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static VkAccelerationStructureBuildSizesInfoKHR build_sizes(
            VkDevice device,
            const VkAccelerationStructureBuildGeometryInfoKHR &build_info,
            span<const u32> max_primitive_counts) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanAccelerationStructure`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkAccelerationStructureKHR vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanAccelerationStructure`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;
        /// Returns the runtime or backend type represented by `VulkanAccelerationStructure`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkAccelerationStructureTypeKHR type() const noexcept;


        /// Returns the current or globally available device address value.
        ///
        /// @return Returns the current device address value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDeviceAddress device_address() const noexcept;

        /// Destroys or releases the `VulkanAccelerationStructure` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkAccelerationStructureKHR acceleration_structure_ = VK_NULL_HANDLE;
        VkAccelerationStructureTypeKHR type_ = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    };

} // namespace SFT::Core::Vulkan
