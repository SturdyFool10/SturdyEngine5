#include <Engine/src/Engine/InputState.hpp>


namespace SFT::Engine {

    /// Returns the current or globally available begin tick value.
    ///
    /// @return Returns the current begin tick value.
    /// @note This function does not throw exceptions.
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

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void InputState::apply(const TextInputEvent &event) noexcept {


        const usize length = strnlen(event.text.utf8, sizeof(event.text.utf8));
        text_this_tick_.append(event.text.utf8, length);


        composing_ = false;
        composition_text_.clear();
    }

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void InputState::apply(const TextEditingEvent &event) noexcept {
        const usize length = strnlen(event.text.utf8, sizeof(event.text.utf8));


        composing_ = length != 0;
        composition_text_.assign(event.text.utf8, length);
    }

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void InputState::apply(const MouseMoveEvent &event) noexcept {
        mouse_x_ = event.mouse.x;
        mouse_y_ = event.mouse.y;


        mouse_delta_x_ += event.mouse.delta_x;
        mouse_delta_y_ += event.mouse.delta_y;
    }

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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

    /// Applies the supplied operation or state to `Engine`.
    ///
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void InputState::apply(const MouseWheelEvent &event) noexcept {
        wheel_delta_x_ += event.wheel.x;
        wheel_delta_y_ += event.wheel.y;
    }

    /// Performs the key down operation for `Engine` using the supplied arguments.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::key_down(KeyboardKey key) const noexcept { return get(key_pressed_, key); }

    /// Performs the key just pressed operation for `Engine` using the supplied arguments.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::key_just_pressed(KeyboardKey key) const noexcept { return get(key_just_pressed_, key); }

    /// Performs the key just released operation for `Engine` using the supplied arguments.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::key_just_released(KeyboardKey key) const noexcept { return get(key_just_released_, key); }

    /// Returns the current or globally available modifiers value.
    ///
    /// @return Returns the current modifiers value.
    /// @note This function does not throw exceptions.
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

    /// Returns the current or globally available text this tick value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::string_view InputState::text_this_tick() const noexcept { return text_this_tick_; }

    /// Returns the current or globally available composition text value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::string_view InputState::composition_text() const noexcept { return composition_text_; }

    /// Returns the current or globally available composing value.
    ///
    /// @return Returns the current composing value.
    /// @note This function does not throw exceptions.
    bool InputState::composing() const noexcept { return composing_; }

    /// Performs the mouse down operation for `Engine` using the supplied arguments.
    ///
    /// @param button `button` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::mouse_down(Platform::Windowing::MouseButton button) const noexcept {
        return get(mouse_pressed_, button);
    }

    /// Performs the mouse just pressed operation for `Engine` using the supplied arguments.
    ///
    /// @param button `button` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::mouse_just_pressed(Platform::Windowing::MouseButton button) const noexcept {
        return get(mouse_just_pressed_, button);
    }

    /// Performs the mouse just released operation for `Engine` using the supplied arguments.
    ///
    /// @param button `button` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::mouse_just_released(Platform::Windowing::MouseButton button) const noexcept {
        return get(mouse_just_released_, button);
    }

    /// Returns the current or globally available mouse x value.
    ///
    /// @return Returns the current mouse x value.
    /// @note This function does not throw exceptions.
    f32 InputState::mouse_x() const noexcept { return mouse_x_; }

    /// Returns the current or globally available mouse y value.
    ///
    /// @return Returns the current mouse y value.
    /// @note This function does not throw exceptions.
    f32 InputState::mouse_y() const noexcept { return mouse_y_; }

    /// Returns the current or globally available mouse delta x value.
    ///
    /// @return Returns the current mouse delta x value.
    /// @note This function does not throw exceptions.
    f32 InputState::mouse_delta_x() const noexcept { return mouse_delta_x_; }

    /// Returns the current or globally available mouse delta y value.
    ///
    /// @return Returns the current mouse delta y value.
    /// @note This function does not throw exceptions.
    f32 InputState::mouse_delta_y() const noexcept { return mouse_delta_y_; }

    /// Returns the current or globally available wheel delta x value.
    ///
    /// @return Returns the current wheel delta x value.
    /// @note This function does not throw exceptions.
    f32 InputState::wheel_delta_x() const noexcept { return wheel_delta_x_; }

    /// Returns the current or globally available wheel delta y value.
    ///
    /// @return Returns the current wheel delta y value.
    /// @note This function does not throw exceptions.
    f32 InputState::wheel_delta_y() const noexcept { return wheel_delta_y_; }

    /// Returns the value or resource currently represented by `Engine`.
    ///
    /// @param bits `bits` value used by the operation.
    /// @param key Key used to identify the requested entry.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::get(const std::vector<bool> &bits, KeyboardKey key) noexcept {
        const usize index = static_cast<usize>(key);
        return index < bits.size() && bits[index];
    }

    /// Returns the value or resource currently represented by `Engine`.
    ///
    /// @param bits `bits` value used by the operation.
    /// @param button `button` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::get(const std::vector<bool> &bits, Platform::Windowing::MouseButton button) noexcept {
        const usize index = static_cast<usize>(button);
        return index < bits.size() && bits[index];
    }

} // namespace SFT::Engine

