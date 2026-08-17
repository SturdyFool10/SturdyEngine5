
#include "EcsUi.hpp"

#include <algorithm>

namespace SFT::Engine {

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiTextInputState::apply(const TextInputEvent &event) noexcept {
        typed_text_ += event.text.utf8;


        composing_ = false;
        composition_text_.clear();
    }

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiTextInputState::apply(const TextEditingEvent &event) noexcept {
        composition_text_ = event.text.utf8;
        composing_ = !composition_text_.empty();
    }

    /// Applies key using the supplied arguments and current state.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void UiTextInputState::apply_key(const KeyboardEvent &event) noexcept {

        if (event.key_code == KeyboardKey::LeftShift || event.key_code == KeyboardKey::RightShift) {
            shift_down_ = event.pressed();
            return;
        }
        if (event.key_code == KeyboardKey::LeftControl || event.key_code == KeyboardKey::RightControl) {
            ctrl_down_ = event.pressed();
            return;
        }
        if (!event.pressed()) {
            return;
        }
        if (ctrl_down_) {
            switch (event.key_code) {
                case KeyboardKey::A: keys_.push_back(UI::EditKey::SelectAll); break;
                case KeyboardKey::C: keys_.push_back(UI::EditKey::Copy); break;
                case KeyboardKey::X: keys_.push_back(UI::EditKey::Cut); break;
                case KeyboardKey::V: keys_.push_back(UI::EditKey::Paste); break;
                default: break;
            }
        }
        switch (event.key_code) {
            case KeyboardKey::Left: keys_.push_back(UI::EditKey::Left); break;
            case KeyboardKey::Right: keys_.push_back(UI::EditKey::Right); break;
            case KeyboardKey::Up: keys_.push_back(UI::EditKey::Up); break;
            case KeyboardKey::Down: keys_.push_back(UI::EditKey::Down); break;
            case KeyboardKey::Home: keys_.push_back(UI::EditKey::Home); break;
            case KeyboardKey::End: keys_.push_back(UI::EditKey::End); break;
            case KeyboardKey::Backspace: keys_.push_back(UI::EditKey::Backspace); break;
            case KeyboardKey::Delete: keys_.push_back(UI::EditKey::Delete); break;
            case KeyboardKey::Enter: keys_.push_back(UI::EditKey::Enter); break;
            case KeyboardKey::Escape: keys_.push_back(UI::EditKey::Escape); break;
            default: break;
        }
    }

    /// Performs the frame input operation for `Engine` using the supplied arguments.
    ///
    /// @param get_clipboard_text `get_clipboard_text` value used by the operation.
    /// @param set_clipboard_text `set_clipboard_text` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    UI::TextEditInput UiTextInputState::frame_input(
        std::function<UString()> get_clipboard_text, std::function<void(const UString &)> set_clipboard_text) const noexcept {
        return UI::TextEditInput{
            .typed_text = typed_text_,
            .keys = keys_,
            .shift_held = shift_down_,
            .word_modifier_held = ctrl_down_,
            .composition_text = composition_text_,
            .composing = composing_,
            .get_clipboard_text = std::move(get_clipboard_text),
            .set_clipboard_text = std::move(set_clipboard_text),
        };
    }

    /// Clears transitions.
    ///
    /// @return Returns the current clear transitions value.
    /// @note This function does not throw exceptions.
    void UiTextInputState::clear_transitions() noexcept {
        typed_text_.clear();
        keys_.clear();
    }

    /// Performs the forward text input state operation for `Engine` using the supplied arguments.
    ///
    /// @param requests `requests` value used by the operation.
    /// @param window Window used or affected by the operation.
    /// @param focus `focus` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void forward_text_input_state(WindowRequests &requests, Platform::Windowing::WindowId window,
                                   std::optional<TextInputFocusInfo> focus) noexcept {
        if (!focus || !focus->ime_enabled) {
            requests.set_text_input_active(window, false);
            return;
        }
        const UI::ElementBounds &field = focus->field_bounds;
        const UI::ElementBounds &caret = focus->caret_bounds;
        requests.set_text_input_area(
            window,
            Platform::Windowing::TextInputArea{
                .x = field.position.x,
                .y = field.position.y,


                .width = std::max(field.size.x, 1.0f),
                .height = std::max(field.size.y, 1.0f),
                .cursor_offset_x = std::max(caret.position.x - field.position.x, 0.0f),
            });
        requests.set_text_input_active(window, true);
    }

} // namespace SFT::Engine
