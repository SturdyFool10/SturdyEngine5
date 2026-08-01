#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <functional>
#include <span>
#pragma endregion

#include "Button.hpp"
#include "Context.hpp"
#include "Style.hpp"

using std::span;

// A combobox-style dropdown, built on Button.hpp (the closed trigger) plus Clay's native floating-
// element support (Style.hpp's FloatingConfig) for the open option list — the same "not a Clay
// primitive" reasoning as every other file here.
namespace SFT::UI {

    // One option's *display* — deliberately just a content-building callback (the same shape
    // MasonryItem::build uses), decoupled from whatever *value* it represents: dropdown() itself
    // only ever deals in indices (`selected_index` in, `DropdownResult::selected_index` out). The
    // caller's own domain type — an enum, a string key, a database row — never has to be
    // representable inside UI:: at all; they look it up from the index on their own side. This is
    // what lets two dropdowns show the same value with entirely different displays (an icon-only
    // trigger vs. a labeled list, a color swatch vs. its hex code, ...) without dropdown() needing
    // to know anything about either.
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

        // A "▾" glyph flush against the trigger's right edge (a growing spacer pushes it there
        // regardless of trigger_decl's own width) — on by default, since a dropdown with no visual
        // affordance that it's a dropdown is a usability trap. Set false to omit it entirely, e.g.
        // if a caller wants to build their own indicator into the selected option's own build()
        // callback instead. `arrow_font_id` must already be registered (Context::register_font())
        // with a font that has U+25BE — this engine's bundled Maple Mono NF does, as do many common
        // UI fonts.
        bool show_arrow_indicator = true;
        Color arrow_color{1.0, 1.0, 1.0, 1.0};
        FontId arrow_font_id = 0;
        u16 arrow_font_size = 14;
    };

    class DropdownState {
      public:
        bool open = false;
        ButtonState trigger_state{};
    };

    struct DropdownResult {
        // True exactly the one frame a *different* option was picked (mirrors Button::clicked's
        // one-frame-edge shape) — `selected_index` is always populated, but only trust it as "new"
        // when `changed` is true; otherwise it just echoes the `selected_index` you passed in.
        bool changed = false;
        usize selected_index = 0;
    };

    // `id` must be stable across frames (same convention as ElementDecl::id) — every option's own
    // hit-test id is derived from it (`id + "#" + index`), so it must not collide with another
    // dropdown's `id` elsewhere in the same tree. `trigger_decl` sizes/styles the closed trigger
    // (background_color/corner_radius/border come from `style.trigger` instead, same override
    // convention as button()); the open list auto-sizes to its widest option and hangs below the
    // trigger's bottom-left corner.
    //
    // Mouse-only for v1 — no keyboard navigation (arrow keys/Enter/Escape) and no "click outside to
    // dismiss" (closes only via re-clicking the trigger or picking an option) — no concrete need for
    // either has come up yet.
    [[nodiscard]] inline DropdownResult dropdown(Context &ctx, const UString &id, const ElementDecl &trigger_decl,
                                                 const DropdownStyle &style, DropdownState &state, f32 delta_seconds,
                                                 usize selected_index, span<const DropdownOption> options) {
        DropdownResult result{.selected_index = selected_index};

        auto anchor = ctx.element(ElementDecl{.sizing = {SizingAxis::fit(), SizingAxis::fit()}});
        (void)anchor;
        {
            ElementDecl trigger = trigger_decl;
            trigger.id = id;
            auto btn = button(ctx, trigger, style.trigger, state.trigger_state, delta_seconds);
            if (btn.clicked) {
                state.open = !state.open;
            }
            if (selected_index < options.size()) {
                options[selected_index].build(ctx);
            }
            if (style.show_arrow_indicator) {
                {
                    auto spacer = ctx.element(ElementDecl{.sizing = {SizingAxis::grow(), SizingAxis::fixed(1.0f)}});
                    (void)spacer;
                }
                ctx.text(u8"▾"_ustr, TextStyle{.color = style.arrow_color, .font_id = style.arrow_font_id,
                                               .font_size = style.arrow_font_size});
            }
        }

        if (state.open) {
            auto list = ctx.element(ElementDecl{
                .sizing = {SizingAxis::fit(), SizingAxis::fit()},
                .direction = LayoutDirection::TopToBottom,
                .background_color = style.list_background,
                .corner_radius = style.corner_radius,
                .border = style.border,
                .floating =
                    FloatingConfig{
                        .attach_to = FloatingAttachTo::Parent,
                        .element_attach_point = FloatingAttachPoint::LeftTop,
                        .parent_attach_point = FloatingAttachPoint::LeftBottom,
                        .offset = {0.0f, 4.0f},
                        .z_index = static_cast<i16>(style.list_z_index),
                    },
            });
            (void)list;
            for (usize i = 0; i < options.size(); ++i) {
                const UString option_id{id.cpp_string() + "#" + std::to_string(i)};
                if (ctx.clicked(option_id)) {
                    result.selected_index = i;
                    result.changed = i != selected_index;
                    state.open = false;
                }
                const bool row_hovered = ctx.hovered(option_id);
                auto row = ctx.element(ElementDecl{
                    .sizing = {SizingAxis::grow(), SizingAxis::fit()},
                    .padding = Padding::symmetric(style.option_padding, style.option_padding / 2),
                    .background_color = row_hovered ? style.option_hovered : Color{0.0, 0.0, 0.0, 0.0},
                    .id = option_id,
                });
                (void)row;
                options[i].build(ctx);
            }
        }
        return result;
    }

} // namespace SFT::UI
