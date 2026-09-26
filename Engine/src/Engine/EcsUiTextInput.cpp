
#include <Engine/EcsUi.hpp>
#include <Engine/ScreenUi.hpp>

#include <algorithm>

namespace SFT::Engine {

    void UiTextInputState::apply(const TextInputEvent &event) noexcept { apply_window_event(input_, event); }

    void UiTextInputState::apply(const TextEditingEvent &event) noexcept { apply_window_event(input_, event); }

    void UiTextInputState::apply_key(const KeyboardEvent &event) noexcept { apply_window_event(input_, event); }

    UI::TextEditInput UiTextInputState::frame_input(std::function<UString()> get_clipboard_text,
                                                    std::function<void(const UString &)> set_clipboard_text) const noexcept {
        return input_.text_input(std::move(get_clipboard_text), std::move(set_clipboard_text));
    }

    void UiTextInputState::clear_transitions() noexcept { input_.end_frame(); }

    /// Performs the forward text input state operation for `Engine` using the supplied arguments.
    ///
    /// @param requests `requests` value used by the operation.
    /// Forwards the focused text field's screen area to the window so the IME candidate window follows the caret,
    /// or turns text input off when nothing is focused.
    ///
    /// @param requests Window requests queue.
    /// @param window Window used or affected by the operation.
    /// @param focus `focus` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void forward_text_input_state(WindowRequests &requests, WindowManager::WindowId window,
                                   std::optional<TextInputFocusInfo> focus) noexcept {
        if (!focus || !focus->ime_enabled) {
            requests.set_text_input_active(window, false);
            return;
        }
        const UI::ElementBounds &field = focus->field_bounds;
        const UI::ElementBounds &caret = focus->caret_bounds;
        requests.set_text_input_area(
            window,
            WindowManager::TextInputArea{
                .x = field.position.x,
                .y = field.position.y,


                .width = std::max(field.size.x, 1.0f),
                .height = std::max(field.size.y, 1.0f),
                .cursor_offset_x = std::max(caret.position.x - field.position.x, 0.0f),
            });
        requests.set_text_input_active(window, true);
    }

} // namespace SFT::Engine
