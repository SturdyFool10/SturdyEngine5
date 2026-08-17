#pragma once

#include <Foundation/src/Foundation.hpp>

namespace SFT::Ecs {


    struct Entity {
        u32 index = 0;
        u32 generation = 0;

        /// Converts the `Entity` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return generation != 0; }
        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(Entity, Entity) noexcept = default;
    };

} // namespace SFT::Ecs
