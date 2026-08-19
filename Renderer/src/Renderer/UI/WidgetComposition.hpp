#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <functional>
#include <optional>
#pragma endregion

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/Style.hpp>


namespace SFT::UI {

    struct PartVisualState {
        bool enabled = true;
        bool hovered = false;
        bool pressed = false;
        bool active = false;
        bool focused = false;
        bool selected = false;
    };


    struct ElementVisualPatch {
        std::optional<Color> background_color;
        std::optional<CornerRadius> corner_radius;
        std::optional<BorderStyle> border;
    };

    struct PartVisualStyle {
        ElementVisualPatch idle{};
        ElementVisualPatch selected{};
        ElementVisualPatch hovered{};
        ElementVisualPatch focused{};
        ElementVisualPatch pressed{};
        ElementVisualPatch active{};
        ElementVisualPatch disabled{};
    };

    /// Applies element visual patch using the supplied arguments and current state.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param patch `patch` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void apply_element_visual_patch(ElementDecl &decl, const ElementVisualPatch &patch) noexcept;


    /// Applies part visual using the supplied arguments and current state.
    ///
    /// @param decl `decl` value used by the operation.
    /// @param style `style` value used by the operation.
    /// @param state `state` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void apply_part_visual(ElementDecl &decl, const PartVisualStyle &style, const PartVisualState &state) noexcept;

    /// Clears element visual.
    ///
    /// @param decl `decl` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void clear_element_visual(ElementDecl &decl) noexcept;

    template <typename BuildContext>
    struct PartSlot {


        bool visible = true;
        bool enabled = true;


        bool render_default = true;
        PartVisualStyle visual{};


        std::function<void(ElementDecl &, const BuildContext &)> alter_decl;

        std::function<void(Context &, const BuildContext &)> build;
    };

} // namespace SFT::UI
