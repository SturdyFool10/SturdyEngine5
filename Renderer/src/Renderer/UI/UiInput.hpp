#pragma once

#include <Foundation/Foundation.hpp>

#include <functional>
#include <glm/vec2.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <Renderer/UI/Context.hpp>
#include <Renderer/UI/TextEdit.hpp>

namespace SFT::UI {

    /// Everything a UI needs to know about its user, as plain data with no idea where it came from.
    ///
    /// A `UiInput` is fed by whatever the application wants to be the "user" of a UI: window mouse and keyboard
    /// events for an on-screen HUD, a ray cast against a quad in the world for an in-world panel, a gamepad
    /// cursor, a replay file, a test. Feed it as events happen, at any rate; a frame's worth of input is consumed
    /// by `UiSurface::begin_frame` (or by passing `pointer()` / `text_input()` to `Context::begin_layout`
    /// yourself), which then calls `end_frame()`.
    ///
    /// Pointer positions are in the UI's own pixel space (top-left origin, +y down): the extent you lay the UI out
    /// at. `Engine::ScreenUi` supplies window coordinates; `UI::ui_pixel_from_uv` supplies world-surface
    /// coordinates.
    class UiInput {
      public:
        // ---- pointer ----
        void move_pointer(glm::vec2 position) noexcept;
        /// Press or release the primary button. A press remembers where it started (drag detection).
        void set_pointer_down(bool down) noexcept;
        /// Cancels an in-progress press (the pointer left the surface, focus was lost).
        void cancel_pointer() noexcept;
        void add_scroll(glm::vec2 delta) noexcept;

        // ---- text and editing keys ----
        /// Committed text. Ends any composition in progress.
        void add_text(std::string_view utf8);
        /// IME pre-edit text; an empty string ends the composition.
        void set_composition(std::string_view utf8);
        void press_key(EditKey key);
        void set_shift_held(bool held) noexcept { shift_held_ = held; }
        /// Ctrl / Cmd: enables word-wise movement in text fields.
        void set_word_modifier_held(bool held) noexcept { word_modifier_held_ = held; }

        // ---- reading ----
        [[nodiscard]] bool shift_held() const noexcept { return shift_held_; }
        [[nodiscard]] bool word_modifier_held() const noexcept { return word_modifier_held_; }
        [[nodiscard]] const PointerState &pointer() const noexcept { return pointer_; }
        [[nodiscard]] TextEditInput text_input(std::function<UString()> get_clipboard_text = nullptr,
                                               std::function<void(const UString &)> set_clipboard_text = nullptr) const;

        /// Whether the UI wanted the pointer last frame (over any element, or holding a drag). Games use it to
        /// keep clicks that land on the UI from also reaching the world.
        [[nodiscard]] bool pointer_consumed() const noexcept { return pointer_consumed_; }
        void set_pointer_consumed(bool consumed) noexcept { pointer_consumed_ = consumed; }

        /// Clears one-frame transitions (press/release edges, scroll, typed text, key presses).
        void end_frame() noexcept;

      private:
        PointerState pointer_{};
        bool pointer_consumed_ = false;
        std::string typed_text_;
        std::string composition_text_;
        bool composing_ = false;
        std::vector<EditKey> keys_;
        bool shift_held_ = false;
        bool word_modifier_held_ = false;
    };

} // namespace SFT::UI
