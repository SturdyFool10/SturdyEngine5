#pragma once

#include <RHI/RHI.hpp>

namespace SFT::Core::Vulkan {


    /// Returns the current or globally available vulkan inventory backend registration value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] RHI::BackendRegistration vulkan_inventory_backend_registration() noexcept;

} // namespace SFT::Core::Vulkan
