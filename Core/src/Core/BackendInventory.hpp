#pragma once

#include <RHI/RHI.hpp>

namespace SFT::Core {

    /// Enumerates physical GPUs across every graphics backend compiled into Core.
    [[nodiscard]] RHI::GpuInventory enumerate_gpu_inventory(const RHI::InstanceDesc &instance_desc);

} // namespace SFT::Core
