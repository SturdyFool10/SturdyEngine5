#include "VulkanAccelerationStructure.hpp"

namespace SFT::Core::Vulkan {

VulkanAccelerationStructure::~VulkanAccelerationStructure() { destroy(); }

VulkanAccelerationStructure::VulkanAccelerationStructure(VulkanAccelerationStructure &&o) noexcept
            : device_(o.device_), acceleration_structure_(o.acceleration_structure_), type_(o.type_) {
            o.device_ = VK_NULL_HANDLE;
            o.acceleration_structure_ = VK_NULL_HANDLE;
        }

VulkanAccelerationStructure &VulkanAccelerationStructure::operator=(VulkanAccelerationStructure &&o) noexcept {
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

[[nodiscard]] RendererExpected<VulkanAccelerationStructure> VulkanAccelerationStructure::create(
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

[[nodiscard]] VkAccelerationStructureBuildSizesInfoKHR VulkanAccelerationStructure::build_sizes(
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

[[nodiscard]] VkAccelerationStructureKHR VulkanAccelerationStructure::vk_handle() const noexcept { return acceleration_structure_; }

[[nodiscard]] bool VulkanAccelerationStructure::is_valid() const noexcept { return acceleration_structure_ != VK_NULL_HANDLE; }

[[nodiscard]] VkAccelerationStructureTypeKHR VulkanAccelerationStructure::type() const noexcept { return type_; }

[[nodiscard]] VkDeviceAddress VulkanAccelerationStructure::device_address() const noexcept {
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

void VulkanAccelerationStructure::destroy() noexcept {
            if (acceleration_structure_ == VK_NULL_HANDLE)
                return;
            if (vkDestroyAccelerationStructureKHR != nullptr) {
                vkDestroyAccelerationStructureKHR(device_, acceleration_structure_, nullptr);
            }
            acceleration_structure_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
