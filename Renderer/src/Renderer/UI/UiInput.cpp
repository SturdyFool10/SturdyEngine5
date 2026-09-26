#include <Renderer/UI/UiInput.hpp>

#include <utility>

namespace SFT::UI {

    void UiInput::move_pointer(glm::vec2 position) noexcept { pointer_.position = position; }

    void UiInput::set_pointer_down(bool down) noexcept {
        if (down != pointer_.down) {
            pointer_.pressed = pointer_.pressed || down;
            pointer_.released = pointer_.released || !down;
            if (down) {
                pointer_.press_position = pointer_.position;
            }
        }
        pointer_.down = down;
    }

    void UiInput::cancel_pointer() noexcept {
        pointer_.cancelled = true;
        pointer_.down = false;
    }

    void UiInput::add_scroll(glm::vec2 delta) noexcept { pointer_.scroll_delta += delta; }

    void UiInput::add_text(std::string_view utf8) {
        typed_text_ += utf8;
        composing_ = false;
        composition_text_.clear();
    }

    void UiInput::set_composition(std::string_view utf8) {
        composition_text_ = utf8;
        composing_ = !composition_text_.empty();
    }

    void UiInput::press_key(EditKey key) { keys_.push_back(key); }

    TextEditInput UiInput::text_input(std::function<UString()> get_clipboard_text,
                                      std::function<void(const UString &)> set_clipboard_text) const {
        return TextEditInput{
            .typed_text = typed_text_,
            .keys = keys_,
            .shift_held = shift_held_,
            .word_modifier_held = word_modifier_held_,
            .composition_text = composition_text_,
            .composing = composing_,
            .get_clipboard_text = std::move(get_clipboard_text),
            .set_clipboard_text = std::move(set_clipboard_text),
        };
    }

    void UiInput::end_frame() noexcept {
        pointer_.pressed = false;
        pointer_.press_position.reset();
        pointer_.released = false;
        pointer_.cancelled = false;
        pointer_.scroll_delta = glm::vec2{0.0f};
        typed_text_.clear();
        keys_.clear();
    }

} // namespace SFT::UI
