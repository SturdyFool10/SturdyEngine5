#pragma once

#include <Foundation/Foundation.hpp>

namespace SFT::Engine {


    struct RenderTargetHandle {
        u64 value = 0;

        /// Reports whether valid holds for this `RenderTargetHandle`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        /// Converts the `RenderTargetHandle` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(RenderTargetHandle, RenderTargetHandle) noexcept = default;
    };


    struct OffscreenRenderTargetDescription {
        u32 width = 0;
        u32 height = 0;
        UString label;
    };

} // namespace SFT::Engine
