#include <Core/Vulkan/VulkanAccelerationStructure.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanAccelerationStructure::~VulkanAccelerationStructure() { destroy(); }

/// Performs the vulkan acceleration structure operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanAccelerationStructure::VulkanAccelerationStructure(VulkanAccelerationStructure &&o) noexcept
            : device_(o.device_), acceleration_structure_(o.acceleration_structure_), type_(o.type_) {
            ZoneScopedN("VulkanAccelerationStructure::VulkanAccelerationStructure");
            o.device_ = VK_NULL_HANDLE;
            o.acceleration_structure_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanAccelerationStructure &VulkanAccelerationStructure::operator=(VulkanAccelerationStructure &&o) noexcept {
            ZoneScopedN("VulkanAccelerationStructure::operator=");
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

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param backing_buffer Buffer used or affected by the operation.
/// @param offset Offset from the beginning of the relevant range or buffer.
/// @param size Requested or available size for the operation.
/// @param type Type value to inspect, select, or convert.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanAccelerationStructure> VulkanAccelerationStructure::create(
            VkDevice device,
            VkBuffer backing_buffer,
            VkDeviceSize offset,
            VkDeviceSize size,
            VkAccelerationStructureTypeKHR type) noexcept {
            ZoneScopedN("VulkanAccelerationStructure::create");
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

/// Builds sizes.
///
/// @param device Device used or affected by the operation.
/// @param build_info Description of the resource or operation to perform.
/// @param max_primitive_counts `max_primitive_counts` value used by the operation.
///
/// @return Returns the value produced by the operation.
/// @note This function does not throw exceptions.
[[nodiscard]] VkAccelerationStructureBuildSizesInfoKHR VulkanAccelerationStructure::build_sizes(
            VkDevice device,
            const VkAccelerationStructureBuildGeometryInfoKHR &build_info,
            span<const u32> max_primitive_counts) noexcept {
            ZoneScopedN("VulkanAccelerationStructure::build_sizes");
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

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkAccelerationStructureKHR VulkanAccelerationStructure::vk_handle() const noexcept { return acceleration_structure_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanAccelerationStructure::is_valid() const noexcept { return acceleration_structure_ != VK_NULL_HANDLE; }

/// Returns the runtime or backend type represented by `Vulkan`.
///
/// @return Returns the current type value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkAccelerationStructureTypeKHR VulkanAccelerationStructure::type() const noexcept { return type_; }

/// Returns the current or globally available device address value.
///
/// @return Returns the current device address value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkDeviceAddress VulkanAccelerationStructure::device_address() const noexcept {
            ZoneScopedN("VulkanAccelerationStructure::device_address");
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

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanAccelerationStructure::destroy() noexcept {
            ZoneScopedN("VulkanAccelerationStructure::destroy");
            if (acceleration_structure_ == VK_NULL_HANDLE)
                return;
            if (vkDestroyAccelerationStructureKHR != nullptr) {
                vkDestroyAccelerationStructureKHR(device_, acceleration_structure_, nullptr);
            }
            acceleration_structure_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
