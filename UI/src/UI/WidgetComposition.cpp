#include <UI/src/UI/WidgetComposition.hpp>


namespace SFT::UI {

    /// Applies element visual patch using the supplied arguments and current state.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param patch `patch` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void apply_element_visual_patch(ElementDecl &decl, const ElementVisualPatch &patch) noexcept {
        if (patch.background_color.has_value())
            decl.background_color = *patch.background_color;
        if (patch.corner_radius.has_value())
            decl.corner_radius = *patch.corner_radius;
        if (patch.border.has_value())
            decl.border = *patch.border;
    }

    /// Applies part visual using the supplied arguments and current state.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void apply_part_visual(ElementDecl &decl, const PartVisualStyle &style, const PartVisualState &state) noexcept {
        apply_element_visual_patch(decl, style.idle);
        if (state.selected)
            apply_element_visual_patch(decl, style.selected);
        if (state.hovered)
            apply_element_visual_patch(decl, style.hovered);
        if (state.focused)
            apply_element_visual_patch(decl, style.focused);
        if (state.pressed)
            apply_element_visual_patch(decl, style.pressed);
        if (state.active)
            apply_element_visual_patch(decl, style.active);
        if (!state.enabled)
            apply_element_visual_patch(decl, style.disabled);
    }

    /// Clears element visual.
    ///
    /// @param decl `decl` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void clear_element_visual(ElementDecl &decl) noexcept {
        decl.background_color = Color{0.0, 0.0, 0.0, 0.0};
        decl.corner_radius = {};
        decl.border = {};
    }

} // namespace SFT::UI

