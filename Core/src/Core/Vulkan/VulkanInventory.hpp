#pragma once

#include <RHI/RHI.hpp>

namespace SFT::Core::Vulkan {

    /// Inventory-only registration. Its adapters expose pre-device metadata and deliberately cannot
    /// create renderer devices; use it with RHI::enumerate_gpu_inventory(), not device selection.
    [[nodiscard]] RHI::BackendRegistration vulkan_inventory_backend_registration() noexcept;

} // namespace SFT::Core::Vulkan
