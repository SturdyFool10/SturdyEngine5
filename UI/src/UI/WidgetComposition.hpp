#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <functional>
#include <optional>
#pragma endregion

#include "Context.hpp"
#include "Style.hpp"

/// Small, renderer-agnostic composition primitives shared by compound widgets. A widget still owns
/// each part's stable identity and Clay scope; callers can selectively hide/disable it, patch the
/// generated visual declaration, alter layout, and add content inside the scope without taking over
/// pointer capture or depending on Clay types.
namespace SFT::UI {

    struct PartVisualState {
        bool enabled = true;
        bool hovered = false;
        bool pressed = false;
        bool active = false;
        bool focused = false;
        bool selected = false;
    };

    /// Optional is required here because transparent colors, zero radii, and empty borders are all
    /// meaningful explicit overrides rather than useful "not set" sentinels.
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

    void apply_element_visual_patch(ElementDecl &decl, const ElementVisualPatch &patch) noexcept;

    /// State patches intentionally compose rather than selecting one enum value. For example, a
    /// selected+hovered option can keep its selected fill while acquiring only the hover border.
    /// Disabled is applied last so it can override any subset of the combined live states.
    void apply_part_visual(ElementDecl &decl, const PartVisualStyle &style, const PartVisualState &state) noexcept;

    void clear_element_visual(ElementDecl &decl) noexcept;

    template <typename BuildContext>
    struct PartSlot {
        /// Invisible parts are not declared. Disabled parts remain visible and receive disabled visual
        /// state, but the owning widget excludes them from interaction.
        bool visible = true;
        bool enabled = true;
        /// Suppresses the widget's built-in decoration/content while retaining its stable scope. The
        /// visual patch and build callback still run, allowing a practical custom replacement.
        bool render_default = true;
        PartVisualStyle visual{};
        /// Called after legacy geometry/style generation and state patching. The widget reasserts the
        /// stable id afterward; every other ElementDecl field is caller-controlled.
        std::function<void(ElementDecl &, const BuildContext &)> alter_decl;
        /// Called inside the part's open scope after optional built-in content.
        std::function<void(Context &, const BuildContext &)> build;
    };

} // namespace SFT::UI
