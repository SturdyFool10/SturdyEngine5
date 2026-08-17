#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <string>
#pragma endregion

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"
#include "WidgetComposition.hpp"

using std::span;

/// A combobox-style dropdown, built on Button.hpp (the closed trigger) plus Clay's native floating-
/// element support (Style.hpp's FloatingConfig) for the open option list — the same "not a Clay
/// primitive" reasoning as every other file here.
namespace SFT::UI {

    /// One option's *display* — deliberately just a content-building callback (the same shape
    /// MasonryItem::build uses), decoupled from whatever *value* it represents: dropdown() itself
    /// only ever deals in indices (`selected_index` in, `DropdownResult::selected_index` out). The
    /// caller's own domain type — an enum, a string key, a database row — never has to be
    /// representable inside UI:: at all; they look it up from the index on their own side. This is
    /// what lets two dropdowns show the same value with entirely different displays (an icon-only
    /// trigger vs. a labeled list, a color swatch vs. its hex code, ...) without dropdown() needing
    /// to know anything about either.
    struct DropdownOption {
        std::function<void(Context &)> build;
    };

    struct DropdownStyle {
        ButtonStyle trigger{};
        Color list_background{0.14, 0.15, 0.19, 0.98};
        Color option_hovered{0.24, 0.26, 0.32, 1.0};
        CornerRadius corner_radius = CornerRadius::all(8.0f);
        BorderStyle border{};
        u16 option_padding = 8;
        u16 list_z_index = 100;

        /// A "▾" glyph, floating-anchored to the trigger (see arrow_attach_point/arrow_offset below)
        /// rather than laid out in the trigger's own flex flow — on by default, since a dropdown with
        /// no visual affordance that it's a dropdown is a usability trap. Set false to omit it
        /// entirely, e.g. if a caller wants to build their own indicator into the selected option's
        /// own build() callback instead. `arrow_font_id` must already be registered
        /// (Context::register_font()) with a font that has U+25BE — this engine's bundled Maple Mono
        /// NF does, as do many common UI fonts.
        bool show_arrow_indicator = true;
        Color arrow_color{1.0, 1.0, 1.0, 1.0};
        FontId arrow_font_id = 0;
        u16 arrow_font_size = 14;

        /// Where the built-in arrow indicator attaches on the trigger, and its pixel offset from
        /// that anchor point — independently overridable (composition.indicator.alter_decl can
        /// still override the resulting FloatingConfig outright, e.g. to move it to the trigger's
        /// left edge or swap it for an SVG glyph entirely via composition.indicator.build). Floating
        /// rather than flex-flow positioning means this placement is correct regardless of
        /// trigger_decl's own direction/child_alignment/content — unlike a spacer-based push, which
        /// silently breaks if the trigger isn't a plain left-to-right row.
        FloatingAttachPoint arrow_attach_point = FloatingAttachPoint::RightCenter;
        glm::vec2 arrow_offset{-10.0f, 0.0f};
        /// Extra padding reserved on the trigger's edge the arrow attaches to, so trigger content
        /// doesn't render underneath the floating indicator. Only applied while the indicator is
        /// actually shown (show_arrow_indicator, or a caller-supplied composition.indicator.build).
        f32 arrow_reserved_space = 22.0f;
        /// Forces the trigger row to vertically center its content by default, independent of
        /// whatever trigger_decl.child_alignment the caller passed — so "dropdown()" looks right out
        /// of the box without every caller having to remember AlignY::Center themselves.
        /// composition.trigger.alter_decl still runs afterward and can override it.
        bool center_trigger_content_vertically = true;
    };

    enum class DropdownVisualPart : u8 {
        Anchor,
        Trigger,
        Indicator,
        List,
        Option,
        Header,
        Footer,
        Empty,
        Tooltip,
    };

    [[nodiscard]] UString dropdown_part_id(const UString &id, DropdownVisualPart part, std::optional<usize> option_index = std::nullopt);

    struct DropdownPartContext {
        DropdownVisualPart part = DropdownVisualPart::Anchor;
        PartVisualState visual{};
        UString id;
        bool open = false;
        usize selected_index = 0;
        std::optional<usize> option_index;
        const DropdownOption *option = nullptr;
        std::optional<ElementBounds> trigger_bounds;
        std::optional<ElementBounds> bounds;
    };

    struct DropdownComposition {
        PartSlot<DropdownPartContext> anchor{};
        PartSlot<DropdownPartContext> trigger{};
        PartSlot<DropdownPartContext> indicator{};
        PartSlot<DropdownPartContext> list{};
        PartSlot<DropdownPartContext> option{};
        PartSlot<DropdownPartContext> header{.visible = false, .render_default = false};
        PartSlot<DropdownPartContext> footer{.visible = false, .render_default = false};
        PartSlot<DropdownPartContext> empty{.visible = false, .render_default = false};
        PartSlot<DropdownPartContext> tooltip{.visible = false, .render_default = false};
        /// nullptr means every option is enabled. The widget-wide and option-slot enabled flags are
        /// still applied before this predicate.
        std::function<bool(usize)> option_enabled;
    };

    class DropdownState {
      public:
        bool open = false;
        ButtonState trigger_state{};
    };

    struct DropdownResult {
        /// True exactly the one frame a *different* option was picked (mirrors Button::clicked's
        /// one-frame-edge shape) — `selected_index` is always populated, but only trust it as "new"
        /// when `changed` is true; otherwise it just echoes the `selected_index` you passed in.
        bool changed = false;
        usize selected_index = 0;
    };

    /// `id` must be stable across frames (same convention as ElementDecl::id) — every option's own
    /// hit-test id is derived from it (`id + "#" + index`), so it must not collide with another
    /// dropdown's `id` elsewhere in the same tree. `trigger_decl` sizes/styles the closed trigger
    /// (background_color/corner_radius/border come from `style.trigger` instead, same override
    /// convention as button()); the open list auto-sizes to its widest option and hangs below the
    /// trigger's bottom-left corner.
    ///
    /// Mouse-only for v1 — no keyboard navigation (arrow keys/Enter/Escape) and no "click outside to
    /// dismiss" (closes only via re-clicking the trigger or picking an option) — no concrete need for
    /// either has come up yet.
    [[nodiscard]] DropdownResult dropdown(Context &ctx, const UString &id, const ElementDecl &trigger_decl, const DropdownStyle &style, DropdownState &state, f32 delta_seconds, usize selected_index, span<const DropdownOption> options, bool enabled, const DropdownComposition &composition);

    [[nodiscard]] DropdownResult dropdown(Context &ctx, const UString &id, const ElementDecl &trigger_decl, const DropdownStyle &style, DropdownState &state, f32 delta_seconds, usize selected_index, span<const DropdownOption> options, bool enabled = true);

} // namespace SFT::UI
