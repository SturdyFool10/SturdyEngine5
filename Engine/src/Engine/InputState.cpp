#include <Engine/src/Engine/InputState.hpp>


namespace SFT::Engine {

    void InputState::begin_tick() noexcept {
        std::fill(key_just_pressed_.begin(), key_just_pressed_.end(), false);
        std::fill(key_just_released_.begin(), key_just_released_.end(), false);
        std::fill(mouse_just_pressed_.begin(), mouse_just_pressed_.end(), false);
        std::fill(mouse_just_released_.begin(), mouse_just_released_.end(), false);
        text_this_tick_.clear();
        mouse_delta_x_ = 0.0f;
        mouse_delta_y_ = 0.0f;
        wheel_delta_x_ = 0.0f;
        wheel_delta_y_ = 0.0f;
    }

    void InputState::apply(const KeyboardEvent &event) noexcept {
        if (event.key_code == KeyboardKey::Unknown) {
            return;
        }
        const usize index = static_cast<usize>(event.key_code);
        if (index >= key_count) {
            return;
        }
        if (event.pressed()) {
            if (!key_pressed_[index]) {
                key_just_pressed_[index] = true;
            }
            key_pressed_[index] = true;
        } else {
            key_pressed_[index] = false;
            key_just_released_[index] = true;
        }
    }

    void InputState::apply(const TextInputEvent &event) noexcept {



        const usize length = strnlen(event.text.utf8, sizeof(event.text.utf8));
        text_this_tick_.append(event.text.utf8, length);



        composing_ = false;
        composition_text_.clear();
    }

    void InputState::apply(const TextEditingEvent &event) noexcept {
        const usize length = strnlen(event.text.utf8, sizeof(event.text.utf8));



        composing_ = length != 0;
        composition_text_.assign(event.text.utf8, length);
    }

    void InputState::apply(const MouseMoveEvent &event) noexcept {
        mouse_x_ = event.mouse.x;
        mouse_y_ = event.mouse.y;


        mouse_delta_x_ += event.mouse.delta_x;
        mouse_delta_y_ += event.mouse.delta_y;
    }

    void InputState::apply(const MouseButtonEvent &event) noexcept {
        if (event.mouse.button_code == Platform::Windowing::MouseButton::Unknown) {
            return;
        }
        const usize index = static_cast<usize>(event.mouse.button_code);
        if (index >= mouse_button_count) {
            return;
        }
        if (event.action == ButtonAction::Pressed) {
            if (!mouse_pressed_[index]) {
                mouse_just_pressed_[index] = true;
            }
            mouse_pressed_[index] = true;
        } else {
            mouse_pressed_[index] = false;
            mouse_just_released_[index] = true;
        }
    }

    void InputState::apply(const MouseWheelEvent &event) noexcept {
        wheel_delta_x_ += event.wheel.x;
        wheel_delta_y_ += event.wheel.y;
    }

    bool InputState::key_down(KeyboardKey key) const noexcept { return get(key_pressed_, key); }

    bool InputState::key_just_pressed(KeyboardKey key) const noexcept { return get(key_just_pressed_, key); }

    bool InputState::key_just_released(KeyboardKey key) const noexcept { return get(key_just_released_, key); }

    Platform::Windowing::KeyModifiers InputState::modifiers() const noexcept {
        using Platform::Windowing::KeyModifiers;
        KeyModifiers mods = KeyModifiers::None;
        if (key_down(KeyboardKey::LeftShift) || key_down(KeyboardKey::RightShift)) {
            mods |= KeyModifiers::Shift;
        }
        if (key_down(KeyboardKey::LeftControl) || key_down(KeyboardKey::RightControl)) {
            mods |= KeyModifiers::Control;
        }
        if (key_down(KeyboardKey::LeftAlt) || key_down(KeyboardKey::RightAlt)) {
            mods |= KeyModifiers::Alt;
        }
        if (key_down(KeyboardKey::LeftSuper) || key_down(KeyboardKey::RightSuper)) {
            mods |= KeyModifiers::Super;
        }
        if (key_down(KeyboardKey::CapsLock)) {
            mods |= KeyModifiers::CapsLock;
        }
        if (key_down(KeyboardKey::NumLock)) {
            mods |= KeyModifiers::NumLock;
        }
        return mods;
    }

    std::string_view InputState::text_this_tick() const noexcept { return text_this_tick_; }

    std::string_view InputState::composition_text() const noexcept { return composition_text_; }

    bool InputState::composing() const noexcept { return composing_; }

    bool InputState::mouse_down(Platform::Windowing::MouseButton button) const noexcept {
        return get(mouse_pressed_, button);
    }

    bool InputState::mouse_just_pressed(Platform::Windowing::MouseButton button) const noexcept {
        return get(mouse_just_pressed_, button);
    }

    bool InputState::mouse_just_released(Platform::Windowing::MouseButton button) const noexcept {
        return get(mouse_just_released_, button);
    }

    f32 InputState::mouse_x() const noexcept { return mouse_x_; }

    f32 InputState::mouse_y() const noexcept { return mouse_y_; }

    f32 InputState::mouse_delta_x() const noexcept { return mouse_delta_x_; }

    f32 InputState::mouse_delta_y() const noexcept { return mouse_delta_y_; }

    f32 InputState::wheel_delta_x() const noexcept { return wheel_delta_x_; }

    f32 InputState::wheel_delta_y() const noexcept { return wheel_delta_y_; }

    bool InputState::get(const std::vector<bool> &bits, KeyboardKey key) noexcept {
        const usize index = static_cast<usize>(key);
        return index < bits.size() && bits[index];
    }

    bool InputState::get(const std::vector<bool> &bits, Platform::Windowing::MouseButton button) noexcept {
        const usize index = static_cast<usize>(button);
        return index < bits.size() && bits[index];
    }

} // namespace SFT::Engine

