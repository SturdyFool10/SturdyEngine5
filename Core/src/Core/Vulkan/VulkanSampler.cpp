#include <Core/Vulkan/VulkanSampler.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

/// Destroys the `Vulkan` and releases resources owned by it.
///
/// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
VulkanSampler::~VulkanSampler() { destroy(); }

/// Performs the vulkan sampler operation for `Vulkan` using the supplied arguments.
///
/// @param o `o` value used by the operation.
///
/// @note This function does not throw exceptions.
VulkanSampler::VulkanSampler(VulkanSampler &&o) noexcept : device_(o.device_), sampler_(o.sampler_) {
            ZoneScopedN("VulkanSampler::VulkanSampler");
            o.device_ = VK_NULL_HANDLE;
            o.sampler_ = VK_NULL_HANDLE;
        }

/// Assigns a new value to this `Vulkan`.
///
/// @param o `o` value used by the operation.
///
/// @return Returns `*this` so the operation can be chained.
/// @note This function does not throw exceptions.
VulkanSampler &VulkanSampler::operator=(VulkanSampler &&o) noexcept {
            ZoneScopedN("VulkanSampler::operator=");
            if (this != &o) {
                destroy();
                device_ = o.device_;
                sampler_ = o.sampler_;
                o.device_ = VK_NULL_HANDLE;
                o.sampler_ = VK_NULL_HANDLE;
            }
            return *this;
        }

/// Creates a `Vulkan` resource or value from the supplied parameters.
///
/// @param device Device used or affected by the operation.
/// @param info Description of the resource or operation to perform.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
/// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
/// @note This function does not throw exceptions.
[[nodiscard]] RendererExpected<VulkanSampler> VulkanSampler::create(
            VkDevice device,
            const VkSamplerCreateInfo &info) noexcept {
            ZoneScopedN("VulkanSampler::create");
            VkSampler sampler = VK_NULL_HANDLE;
            if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS)
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "vkCreateSampler failed.");
            VulkanSampler out;
            out.device_ = device;
            out.sampler_ = sampler;
            return out;
        }

/// Returns the Vulkan handle associated with this `Vulkan`.
///
/// @return Returns the current Vulkan handle value.
/// @note This function does not throw exceptions.
[[nodiscard]] VkSampler VulkanSampler::vk_handle() const noexcept { return sampler_; }

/// Reports whether valid holds for this `Vulkan`.
///
/// @return Returns the current is valid value.
/// @note This function does not throw exceptions.
[[nodiscard]] bool VulkanSampler::is_valid() const noexcept { return sampler_ != VK_NULL_HANDLE; }

/// Destroys or releases the `Vulkan` resource represented by the supplied parameters.
///
/// @return Returns the current destroy value.
/// @note This function does not throw exceptions.
void VulkanSampler::destroy() noexcept {
            ZoneScopedN("VulkanSampler::destroy");
            if (sampler_ == VK_NULL_HANDLE)
                return;
            vkDestroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
            device_ = VK_NULL_HANDLE;
        }

} // namespace SFT::Core::Vulkan
