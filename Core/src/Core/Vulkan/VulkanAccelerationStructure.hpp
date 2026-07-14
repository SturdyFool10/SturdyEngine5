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

    // Owns a VkAccelerationStructureKHR (a BLAS or TLAS) — the ray tracing traversal object. The AS
    // object is a *view* over caller-owned backing memory: you first size it with build_sizes(), create
    // a VulkanBuffer of AccelerationStructure usage for the backing store, then create() the AS over a
    // (buffer, offset, size) range. Builds are recorded as GPU work with caller-owned scratch (see
    // VulkanCommandBuffer::build_acceleration_structures); this type owns only the AS handle, never the
    // backing buffer or scratch — mirroring how Vulkan separates the structure from its memory.
    class VulkanAccelerationStructure {
      public:
        VulkanAccelerationStructure() = default;
        ~VulkanAccelerationStructure() { destroy(); }

        VulkanAccelerationStructure(const VulkanAccelerationStructure &) = delete;
        VulkanAccelerationStructure &operator=(const VulkanAccelerationStructure &) = delete;

        VulkanAccelerationStructure(VulkanAccelerationStructure &&o) noexcept
            : device_(o.device_), acceleration_structure_(o.acceleration_structure_), type_(o.type_) {
            o.device_ = VK_NULL_HANDLE;
            o.acceleration_structure_ = VK_NULL_HANDLE;
        }
        VulkanAccelerationStructure &operator=(VulkanAccelerationStructure &&o) noexcept {
            if (this != &o) {
                destroy();
                device_ = o.device_;
                acceleration_structure_ = o.acceleration_structure_;
                type_ = o.type_;
                o.device_ = VK_NULL_HANDLE;
                o.acceleration_structure_ = VK_NULL_HANDLE;
            }
            return *this;
        }

        // Creates the AS over a range of a caller-owned backing buffer (which must carry
        // VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR and outlive this object).
        [[nodiscard]] static RendererExpected<VulkanAccelerationStructure> create(
            VkDevice device,
            VkBuffer backing_buffer,
            VkDeviceSize offset,
            VkDeviceSize size,
            VkAccelerationStructureTypeKHR type) noexcept {
            if (vkCreateAccelerationStructureKHR == nullptr)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "vkCreateAccelerationStructureKHR is not loaded (acceleration structure extension not enabled).");
            VkAccelerationStructureCreateInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                .pNext = nullptr,
                .createFlags = 0,
                .buffer = backing_buffer,
                .offset = offset,
                .size = size,
                .type = type,
                .deviceAddress = 0,
            };
            VkAccelerationStructureKHR as = VK_NULL_HANDLE;
            if (vkCreateAccelerationStructureKHR(device, &info, nullptr, &as) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateAccelerationStructureKHR failed.");
            VulkanAccelerationStructure out;
            out.device_ = device;
            out.acceleration_structure_ = as;
            out.type_ = type;
            return out;
        }

        // Queries the backing-store and scratch sizes for a build before any memory is allocated —
        // `max_primitive_counts` gives the primitive count per geometry in `build_info`.
        [[nodiscard]] static VkAccelerationStructureBuildSizesInfoKHR build_sizes(
            VkDevice device,
            const VkAccelerationStructureBuildGeometryInfoKHR &build_info,
            span<const u32> max_primitive_counts) noexcept {
            VkAccelerationStructureBuildSizesInfoKHR sizes{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
                .pNext = nullptr,
            };
            if (vkGetAccelerationStructureBuildSizesKHR != nullptr) {
                vkGetAccelerationStructureBuildSizesKHR(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                        &build_info, max_primitive_counts.data(), &sizes);
            }
            return sizes;
        }

        [[nodiscard]] VkAccelerationStructureKHR vk_handle() const noexcept { return acceleration_structure_; }
        [[nodiscard]] bool is_valid() const noexcept { return acceleration_structure_ != VK_NULL_HANDLE; }
        [[nodiscard]] VkAccelerationStructureTypeKHR type() const noexcept { return type_; }

        // The device address a TLAS instance record stores to reference a BLAS, and what the build
        // geometry info uses as its `dstAccelerationStructure` address.
        [[nodiscard]] VkDeviceAddress device_address() const noexcept {
            if (vkGetAccelerationStructureDeviceAddressKHR == nullptr) {
                return 0;
            }
            VkAccelerationStructureDeviceAddressInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .pNext = nullptr,
                .accelerationStructure = acceleration_structure_,
            };
            return vkGetAccelerationStructureDeviceAddressKHR(device_, &info);
        }

        void destroy() noexcept {
            if (acceleration_structure_ == VK_NULL_HANDLE)
                return;
            if (vkDestroyAccelerationStructureKHR != nullptr) {
                vkDestroyAccelerationStructureKHR(device_, acceleration_structure_, nullptr);
            }
            acceleration_structure_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkAccelerationStructureKHR acceleration_structure_ = VK_NULL_HANDLE;
        VkAccelerationStructureTypeKHR type_ = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    };

} // namespace SFT::Core::Vulkan
