#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <optional>
#pragma endregion

#include <Platform/Window/WindowNative.hpp>

namespace SFT::Platform::Windowing::Detail {

    /// Queries the compositor's preferred image description for this wl_surface and returns its
    /// reference-white luminance in nits. Uses a private Wayland event queue so SDL's queue is never
    /// dispatched by this integration path.
    [[nodiscard]] std::optional<f32> query_wayland_surface_reference_white(
        NativeWindowHandle handle) noexcept;

} // namespace SFT::Platform::Windowing::Detail
