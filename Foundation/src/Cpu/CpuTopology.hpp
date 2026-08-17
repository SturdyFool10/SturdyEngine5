#pragma once

#include <Foundation/src/Types.hpp>

namespace SFT::Foundation::Cpu {


    enum class CoreType {
        Unknown,
        Efficiency,
        Performance,
    };

    struct CurrentCore {


        u32 x2apic_id = 0;
        CoreType type = CoreType::Unknown;
    };


    /// Returns the current or globally available current core value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] CurrentCore current_core() noexcept;

} // namespace SFT::Foundation::Cpu
