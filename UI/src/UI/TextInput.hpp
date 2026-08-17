#pragma once

#include <Foundation/src/Foundation.hpp>

#include "Context.hpp"
#include "Style.hpp"
#include "TextEdit.hpp"

/// Single-line text input, built on TextEdit.hpp's shared buffer/caret/selection engine — same
/// "not a Clay primitive" reasoning as Button/Toggle/Dropdown.
namespace SFT::UI {

    struct TextInputResult {
        bool changed = false;
        /// True the one frame EditKey::Enter was pressed while focused (see TextEditState::
        /// apply_input()'s own doc comment — single-line inputs treat Enter as "submit", not as a
        /// literal newline).
        bool submitted = false;
        bool focused = false;
        /// The caret's own on-screen rectangle (one frame stale, same as every other
        /// Context::element_bounds() read — see this field's own use in the function body), present
        /// only while focused. Feed it to Platform::Windowing::Window::set_text_input_area() (via
        /// whatever request-queue bridge the embedding app uses — see Engine::WindowRequests::
        /// set_text_input_area() for this engine's own) so an IME's composition candidate window
        /// anchors to the caret instead of a default/wrong location.
        std::optional<ElementBounds> caret_bounds;
    };

    /// `decl.id` must be set (same convention as button()) — it's the click-to-focus hit-test id.
    /// Set `decl.sizing`/`.padding`/`.child_alignment` (typically `{Left, Center}` for a
    /// single-line box) yourself, same as every other widget here — text_input() only overwrites
    /// `background_color`/`corner_radius`/`border`, driven by the current hover/focus/disabled
    /// state.
    ///
    /// Click-to-focus *and* click-to-position: clicking the input focuses it and places the caret at
    /// the clicked character, found via Context::hit_test_text_byte_offset() against the single
    /// line's own on-screen bounds (Detail::hit_test_line_scalar(), TextEdit.hpp) — falls back to
    /// the end of the text only if that line wasn't rendered last frame yet (e.g. the very first
    /// frame this input ever appears, before Context has any committed bounds for it to hit-test
    /// against). Clicking anywhere else in the UI (via Context::clicked_outside()) drops focus.
    [[nodiscard]] TextInputResult text_input(Context &ctx, const ElementDecl &decl, const TextEditStyle &style,
                                                     TextEditState &state, const TextEditInput &input,
                                                     f32 delta_seconds, const UString &placeholder = {},
                                                     bool enabled = true);

} // namespace SFT::UI
