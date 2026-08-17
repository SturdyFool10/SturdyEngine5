#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Platform/Platform.hpp>

#include <glm/vec2.hpp>

namespace SFT::Core {


    enum class SurfaceProvider {
        Unknown,
        Native,
        SDL3,
        GLFW,
    };


    enum class SurfaceSystem {
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
    };


    struct RenderSurfaceDescriptor {
        SurfaceProvider provider = SurfaceProvider::Unknown;
        SurfaceSystem system = SurfaceSystem::Unknown;
        void *display = nullptr;
        void *window = nullptr;
    };


    using Extent2D = glm::uvec2;

    /// Reports whether zero holds.
    ///
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr bool is_zero(Extent2D extent) noexcept {
        return extent.x == 0 || extent.y == 0;
    }


    struct RenderSurfaceHandle {
        Platform::Windowing::WindowId window_id = Platform::Windowing::invalid_window_id;

        /// Reports whether valid holds for this `RenderSurfaceHandle`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return window_id != Platform::Windowing::invalid_window_id;
        }

        /// Compares the operands for equality.
        ///
        /// @return Returns `true` when the operands compare equal; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        friend constexpr bool operator==(RenderSurfaceHandle, RenderSurfaceHandle) noexcept = default;
    };

} // namespace SFT::Core
