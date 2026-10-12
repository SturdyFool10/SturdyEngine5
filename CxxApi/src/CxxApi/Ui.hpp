#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <Engine/Engine.hpp>

#include <CxxApi/Runtime.hpp>
#include <CxxApi/Types/Ui.hpp>

/// The immediate-mode UI, drawn from inside `GameLogicCallbacks::request_render_frame` (shared with the C ABI through
/// `Engine/ForeignUi.hpp`, so every language gets the same behaviour). There is no draw callback: the frame callback *is* the draw
/// call. Open a UI frame, describe elements and text, close the frame, and the layout is attached to the frame as an overlay.
///
///     ui_begin(frame);
///     ui_begin_element({.width_kind = UiSizing::Fixed, .width_value = 240, .padding = {8, 8, 8, 8},
///                       .background = {0.1f, 0.1f, 0.12f, 0.9f}}, "panel");
///     ui_text("Health: 87", {});
///     ui_end_element();
///     ui_end(frame);
///
/// A UI frame belongs to the thread that opened it (a window's render thread, usually) and only one can be open per thread. Every
/// `ui_begin_element` needs an `ui_end_element` before `ui_end`; a frame closed with an element open reports it. Every function throws
/// `Error` when something is wrong (no UI frame open, no graphics device, an unreadable font).
namespace SFT::CxxApi {

    /// Loads a font file and registers it under `font_id`. Safe at any time, including before the first frame.
    void ui_register_font(Engine::Engine &engine, const std::string &path, std::uint32_t font_id);
    void ui_set_scale_mode(Engine::Engine &engine, UiScaleMode mode);

    /// Opens a UI frame sized to this frame's surface.
    void ui_begin(FrameBuilder &frame);
    /// Closes the UI frame and attaches it to `frame` as an overlay. Throws (and still closes the frame) when an element was left open.
    void ui_end(FrameBuilder &frame);
    /// Closes the UI frame without attaching anything, so the next one starts clean.
    void ui_end_discard(FrameBuilder &frame);

    /// Opens an element; `id` (may be empty) names it for `ui_hovered` / `ui_clicked`.
    void ui_begin_element(const UiElement &element, const std::string &id);
    void ui_end_element();
    void ui_text(const std::string &text, const UiTextStyle &style);

    /// Whether the pointer is over / was clicked on the element with this id, as laid out so far this frame.
    [[nodiscard]] bool ui_hovered(const std::string &id);
    [[nodiscard]] bool ui_clicked(const std::string &id);
    /// The pointer in UI units.
    [[nodiscard]] std::array<float, 2> ui_pointer_position();
    [[nodiscard]] bool ui_pointer_down();

} // namespace SFT::CxxApi
