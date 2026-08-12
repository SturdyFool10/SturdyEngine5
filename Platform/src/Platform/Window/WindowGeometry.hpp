#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <glm/vec2.hpp>
#pragma endregion

namespace SFT::Platform::Windowing {

    using WindowExtent = glm::u32vec2;
    using WindowPosition = glm::i32vec2;

    // A focused text field's on-screen rectangle, in the window's own client-area pixel coordinates
    // (top-left origin, same space WindowMouseMoveEvent::x/y use) — tells the OS/IME where to anchor
    // a composition candidate window. `cursor_offset_x` is the caret's own horizontal offset from
    // `x` (not just the field's left edge), so the candidate popup can anchor to the caret itself
    // rather than the whole field when they differ (scrolled text, existing content before the
    // caret, ...). See Window::set_text_input_area()'s own doc comment.
    struct TextInputArea {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 width = 0.0f;
        f32 height = 0.0f;
        f32 cursor_offset_x = 0.0f;
    };

} // namespace SFT::Platform::Windowing
