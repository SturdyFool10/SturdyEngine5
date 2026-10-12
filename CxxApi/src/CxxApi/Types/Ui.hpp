#pragma once

// Plain-data types for the immediate-mode UI. bindgen-safe: <cstddef>/<cstdint> only (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// How one axis of a UI element is sized. Mirrors `Engine::ForeignUiSizing`; values never change.
    enum class UiSizing : std::uint32_t {
        /// Just big enough for its children.
        Fit = 0,
        /// Shares the remaining space with its siblings.
        Grow = 1,
        /// `*_value` logical units.
        Fixed = 2,
        /// `*_value` as a fraction (0..1) of the parent.
        Percent = 3,
    };

    enum class UiDirection : std::uint32_t {
        LeftToRight = 0,
        TopToBottom = 1,
    };

    /// Whether UI units follow the OS content scale (the default) or are raw pixels.
    enum class UiScaleMode : std::uint32_t {
        ContentScale = 0,
        RawPixels = 1,
    };

    /// One layout element. The element's id (for `ui_hovered` / `ui_clicked`) is passed separately, because plain data has no strings.
    struct UiElement {
        UiSizing width_kind = UiSizing::Fit;
        float width_value = 0.0f;
        UiSizing height_kind = UiSizing::Fit;
        float height_value = 0.0f;
        /// Left, right, top, bottom, in logical UI units.
        float padding[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float child_gap = 0.0f;
        UiDirection direction = UiDirection::LeftToRight;
        /// Straight-alpha non-linear sRGB; alpha 0 draws no background.
        float background[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        /// Top-left, top-right, bottom-right, bottom-left.
        float corner_radius[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    struct UiTextStyle {
        std::uint32_t font_id = 0;
        std::uint32_t font_size = 16;
        /// Straight-alpha non-linear sRGB.
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };

} // namespace SFT::CxxApi
