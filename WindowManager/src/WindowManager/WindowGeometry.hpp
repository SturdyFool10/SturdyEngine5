#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>

#include <algorithm>
#include <cmath>
#pragma endregion

namespace SFT::WindowManager {

    using WindowExtent = glm::u32vec2;
    using WindowPosition = glm::i32vec2;

                                                                                                     
                                                                                                        
                                                                                                   
                                                                                                   
                                                                                                
                                                                         
    /// IME text-input rectangle in physical (framebuffer) pixels relative to the window's client area;
    /// providers convert to their native coordinate space.
    struct TextInputArea {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
        f32 cursor_offset_x = 0.0f;
    };

    /// Physical pixels per window-coordinate unit: 1 on Win32/X11, the output scale on macOS/scaled
    /// Wayland/Web. Distinct from content scale (the user's UI-scale preference), which can be 2 on
    /// Windows while density stays 1.
    ///
    /// @param size Window size in the provider's window coordinates.
    /// @param framebuffer Framebuffer size in physical pixels.
    ///
    /// @return `framebuffer / size` per axis, or 1 when either is empty.
    [[nodiscard]] inline glm::vec2 pixel_density(WindowExtent size, WindowExtent framebuffer) noexcept {
        if (size.x == 0 || size.y == 0 || framebuffer.x == 0 || framebuffer.y == 0) {
            return glm::vec2{1.0f};
        }
        return glm::vec2{framebuffer} / glm::vec2{size};
    }

    /// Converts a physical-pixel extent into window coordinates for a window with the given density.
    ///
    /// @param extent Extent in physical pixels.
    /// @param density Pixel density as returned by `pixel_density()`.
    ///
    /// @return The rounded window-coordinate extent, never smaller than 1x1.
    [[nodiscard]] inline WindowExtent physical_to_window_extent(WindowExtent extent, glm::vec2 density) noexcept {
        const auto axis = [](u32 value, f32 scale) -> u32 {
            if (!(scale > 0.0f) || !std::isfinite(scale)) {
                return value;
            }
            return static_cast<u32>(std::max(1.0f, std::round(static_cast<f32>(value) / scale)));
        };
        return WindowExtent{axis(extent.x, density.x), axis(extent.y, density.y)};
    }

} // namespace SFT::WindowManager
