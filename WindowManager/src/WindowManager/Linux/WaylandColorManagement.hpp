#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <optional>
#pragma endregion

#include <WindowManager/WindowNative.hpp>

namespace SFT::WindowManager::Detail {


    /// Queries wayland surface reference white from the active backend or runtime state.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::optional<f32> query_wayland_surface_reference_white(
        NativeWindowHandle handle) noexcept;

} // namespace SFT::WindowManager::Detail
