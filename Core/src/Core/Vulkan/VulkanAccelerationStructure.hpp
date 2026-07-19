#pragma once

#include <Foundation/src/Foundation.hpp>
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

    // Owns a VkAccelerationStructureKHR (a BLAS or TLAS) — the ray tracing traversal object. The AS
    // object is a *view* over caller-owned backing memory: you first size it with build_sizes(), create
    // a VulkanBuffer of AccelerationStructure usage for the backing store, then create() the AS over a
    // (buffer, offset, size) range. Builds are recorded as GPU work with caller-owned scratch (see
    // VulkanCommandBuffer::build_acceleration_structures); this type owns only the AS handle, never the
    // backing buffer or scratch — mirroring how Vulkan separates the structure from its memory.
    class VulkanAccelerationStructure {
      public:
        VulkanAccelerationStructure() = default;
        ~VulkanAccelerationStructure();

        VulkanAccelerationStructure(const VulkanAccelerationStructure &) = delete;
        VulkanAccelerationStructure &operator=(const VulkanAccelerationStructure &) = delete;

        VulkanAccelerationStructure(VulkanAccelerationStructure &&o) noexcept;
        VulkanAccelerationStructure &operator=(VulkanAccelerationStructure &&o) noexcept;

        // Creates the AS over a range of a caller-owned backing buffer (which must carry
        // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR and outlive this object).
        [[nodiscard]] static RendererExpected<VulkanAccelerationStructure> create(
            VkDevice device,
            VkBuffer backing_buffer,
            VkDeviceSize offset,
            VkDeviceSize size,
            VkAccelerationStructureTypeKHR type) noexcept;

        // Queries the backing-store and scratch sizes for a build before any memory is allocated —
        // `max_primitive_counts` gives the primitive count per geometry in `build_info`.
        [[nodiscard]] static VkAccelerationStructureBuildSizesInfoKHR build_sizes(
            VkDevice device,
            const VkAccelerationStructureBuildGeometryInfoKHR &build_info,
            span<const u32> max_primitive_counts) noexcept;

        [[nodiscard]] VkAccelerationStructureKHR vk_handle() const noexcept;
        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] VkAccelerationStructureTypeKHR type() const noexcept;

        // The device address a TLAS instance record stores to reference a BLAS, and what the build
        // geometry info uses as its `dstAccelerationStructure` address.
        [[nodiscard]] VkDeviceAddress device_address() const noexcept;

        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkAccelerationStructureKHR acceleration_structure_ = VK_NULL_HANDLE;
        VkAccelerationStructureTypeKHR type_ = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    };

} // namespace SFT::Core::Vulkan
