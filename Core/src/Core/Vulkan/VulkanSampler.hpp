#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include "volk.h"
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

using SFT::Core::graphics_backend_error;
using SFT::Core::GraphicsBackendErrorCode;
using SFT::Core::RendererExpected;

namespace SFT::Core::Vulkan {

    class VulkanSampler {
      public:
        /// Constructs a `VulkanSampler` in its default state.
        ///
        /// @note This function does not throw exceptions.
        VulkanSampler() = default;
        /// Destroys the `VulkanSampler` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanSampler();

        /// Disables this construction form for `VulkanSampler`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSampler(const VulkanSampler &) = delete;
        /// Assigns a new value to this `VulkanSampler`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        VulkanSampler &operator=(const VulkanSampler &) = delete;

        /// Constructs a `VulkanSampler` from the supplied initialization values.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        VulkanSampler(VulkanSampler &&o) noexcept;
        /// Assigns a new value to this `VulkanSampler`.
        ///
        /// @param o `o` value used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        VulkanSampler &operator=(VulkanSampler &&o) noexcept;

        /// Creates a `VulkanSampler` resource or value from the supplied parameters.
        ///
        /// @param device Device used or affected by the operation.
        /// @param info Description of the resource or operation to perform.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static RendererExpected<VulkanSampler> create(
            VkDevice device,
            const VkSamplerCreateInfo &info) noexcept;

        /// Returns the Vulkan handle associated with this `VulkanSampler`.
        ///
        /// @return Returns the current Vulkan handle value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkSampler vk_handle() const noexcept;
        /// Reports whether valid holds for this `VulkanSampler`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_valid() const noexcept;

        /// Destroys or releases the `VulkanSampler` resource represented by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy() noexcept;

      private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkSampler sampler_ = VK_NULL_HANDLE;
    };

} // namespace SFT::Core::Vulkan
