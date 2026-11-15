#pragma once

#include <Engine/ForeignUi.hpp>
#include <FFI/Sturdy.h>

namespace SFT::Ffi {

    /// Copies an ABI element into the shared description.
    [[nodiscard]] inline SFT::Engine::ForeignUiElement to_foreign_element(const SturdyUiElement &element) {
        SFT::Engine::ForeignUiElement out;
        out.width_kind = static_cast<SFT::Engine::ForeignUiSizing>(element.width_kind);
        out.width_value = element.width_value;
        out.height_kind = static_cast<SFT::Engine::ForeignUiSizing>(element.height_kind);
        out.height_value = element.height_value;
        out.padding[0] = element.padding_left;
        out.padding[1] = element.padding_right;
        out.padding[2] = element.padding_top;
        out.padding[3] = element.padding_bottom;
        out.child_gap = element.child_gap;
        out.top_to_bottom = element.direction == STURDY_UI_DIRECTION_TOP_TO_BOTTOM;
        for (int i = 0; i < 4; ++i) {
            out.background[i] = element.background[i];
            out.corner_radius[i] = element.corner_radius[i];
        }
        if (element.id != nullptr) {
            out.id = element.id;
        }
        return out;
    }

    /// Copies an ABI text style into the shared description.
    [[nodiscard]] inline SFT::Engine::ForeignUiTextStyle to_foreign_text_style(const SturdyUiTextStyle &style) {
        SFT::Engine::ForeignUiTextStyle out;
        out.font_id = style.font_id;
        out.font_size = style.font_size;
        for (int i = 0; i < 4; ++i) {
            out.color[i] = style.color[i];
            out.decoration_color[i] = style.decoration_color[i];
            out.shadow_color[i] = style.shadow_color[i];
        }
        out.decoration_lines = style.decoration_lines;
        out.decoration_style = style.decoration_style;
        out.decoration_thickness = style.decoration_thickness;
        out.has_decoration_color = style.has_decoration_color != 0;
        out.shadow_enabled = style.shadow_enabled != 0;
        out.shadow_offset[0] = style.shadow_offset[0];
        out.shadow_offset[1] = style.shadow_offset[1];
        return out;
    }

} // namespace SFT::Ffi
