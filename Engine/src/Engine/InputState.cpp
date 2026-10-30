#include <Engine/InputState.hpp>


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
        // A finger that lifted last tick has been seen by now; the rest only lose their "just" flags and motion.
        std::erase_if(touches_, [](const TouchPoint &touch) { return !touch.down && touch.just_released; });
        for (TouchPoint &touch : touches_) {
            touch.just_pressed = false;
            touch.just_released = false;
            touch.dx = 0.0f;
            touch.dy = 0.0f;
        }
        std::erase_if(pens_, [](const PenPoint &pen) { return !pen.in_range; });
        for (PenPoint &pen : pens_) {
            pen.just_down = false;
            pen.just_up = false;
        }
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
        if (event.mouse.button_code == WindowManager::MouseButton::Unknown) {
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
    WindowManager::KeyModifiers InputState::modifiers() const noexcept {
        using WindowManager::KeyModifiers;
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
    bool InputState::mouse_down(WindowManager::MouseButton button) const noexcept {
        return get(mouse_pressed_, button);
    }

    /// Performs the mouse just pressed operation for `Engine` using the supplied arguments.
    ///
    /// @param button `button` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::mouse_just_pressed(WindowManager::MouseButton button) const noexcept {
        return get(mouse_just_pressed_, button);
    }

    /// Performs the mouse just released operation for `Engine` using the supplied arguments.
    ///
    /// @param button `button` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    bool InputState::mouse_just_released(WindowManager::MouseButton button) const noexcept {
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
    bool InputState::get(const std::vector<bool> &bits, WindowManager::MouseButton button) noexcept {
        const usize index = static_cast<usize>(button);
        return index < bits.size() && bits[index];
    }

    void InputState::apply(const TouchEvent &event) noexcept {
        using Kind = WindowManager::WindowEventKind;
        const auto found = std::find_if(touches_.begin(), touches_.end(), [&](const TouchPoint &point) {
            return point.device == event.touch.device && point.finger == event.touch.finger && !(point.just_released && !point.down);
        });
        TouchPoint *point = found != touches_.end() ? &*found : nullptr;
        if (point == nullptr) {
            if (event.kind != Kind::TouchDown && event.kind != Kind::TouchMotion) {
                return; // a lift or cancel for a finger we never saw go down
            }
            // A motion with no earlier down (the window opened under a finger) still starts a touch.
            touches_.push_back(TouchPoint{.window = event.window, .device = event.touch.device, .finger = event.touch.finger});
            point = &touches_.back();
            point->down = true;
            point->just_pressed = true;
        }
        point->window = event.window;
        point->dx += event.touch.dx;
        point->dy += event.touch.dy;
        point->x = event.touch.x;
        point->y = event.touch.y;
        point->pressure = event.touch.pressure;
        if (event.kind == Kind::TouchUp || event.kind == Kind::TouchCanceled) {
            point->down = false;
            point->just_released = true;
            point->cancelled = event.kind == Kind::TouchCanceled;
        }
    }

    void InputState::apply(const PenEvent &event) noexcept {
        using Kind = WindowManager::WindowEventKind;
        auto found = std::find_if(pens_.begin(), pens_.end(), [&](const PenPoint &point) { return point.pen == event.pen.pen; });
        if (found == pens_.end()) {
            pens_.push_back(PenPoint{.window = event.window, .pen = event.pen.pen});
            found = pens_.end() - 1;
            found->in_range = true; // any pen event means it is in range
        }
        PenPoint &pen = *found;
        pen.window = event.window;
        switch (event.kind) {
            case Kind::PenProximityIn: pen.in_range = true; break;
            case Kind::PenProximityOut:
                pen.in_range = false;
                if (pen.down) {
                    pen.down = false;
                    pen.just_up = true;
                }
                break;
            case Kind::PenDown:
                pen.x = event.pen.x;
                pen.y = event.pen.y;
                pen.eraser = event.pen.eraser;
                if (!pen.down) {
                    pen.just_down = true;
                }
                pen.down = true;
                break;
            case Kind::PenUp:
                pen.x = event.pen.x;
                pen.y = event.pen.y;
                pen.eraser = event.pen.eraser;
                if (pen.down) {
                    pen.just_up = true;
                }
                pen.down = false;
                break;
            case Kind::PenMotion:
                pen.x = event.pen.x;
                pen.y = event.pen.y;
                break;
            case Kind::PenButtonDown:
            case Kind::PenButtonUp:
                if (event.pen.button >= 1 && event.pen.button <= 32) {
                    const u32 bit = 1u << (event.pen.button - 1);
                    pen.buttons = event.kind == Kind::PenButtonDown ? (pen.buttons | bit) : (pen.buttons & ~bit);
                }
                break;
            case Kind::PenAxis:
                pen.x = event.pen.x;
                pen.y = event.pen.y;
                switch (event.pen.axis) {
                    case WindowManager::PenAxis::Pressure: pen.pressure = event.pen.value; break;
                    case WindowManager::PenAxis::XTilt: pen.x_tilt = event.pen.value; break;
                    case WindowManager::PenAxis::YTilt: pen.y_tilt = event.pen.value; break;
                    case WindowManager::PenAxis::Distance: pen.distance = event.pen.value; break;
                    case WindowManager::PenAxis::Rotation: pen.rotation = event.pen.value; break;
                    case WindowManager::PenAxis::Slider: pen.slider = event.pen.value; break;
                    case WindowManager::PenAxis::TangentialPressure: pen.tangential_pressure = event.pen.value; break;
                }
                break;
            default: break;
        }
    }

    void InputState::poll_gamepads() noexcept {
        gamepad_events_.clear();
        if (!gamepads_enabled_) {
            gamepad_frames_.clear();
            return;
        }
        gamepad_events_ = Gamepads::take_events();
        for (const Gamepads::Event &event : gamepad_events_) {
            if (event.kind == Gamepads::EventKind::Disconnected) {
                std::erase_if(gamepad_frames_, [&](const GamepadFrame &frame) { return frame.id == event.id; });
            }
        }
        // Every connected id is sampled; an id that is not a mapped gamepad has no state and is skipped.
        std::vector<u32> ids;
        for (const GamepadFrame &frame : gamepad_frames_) {
            ids.push_back(frame.id);
        }
        for (const Gamepads::Event &event : gamepad_events_) {
            if (event.kind == Gamepads::EventKind::Connected && std::find(ids.begin(), ids.end(), event.id) == ids.end()) {
                ids.push_back(event.id);
            }
        }
        std::vector<GamepadFrame> next;
        next.reserve(ids.size());
        for (const u32 id : ids) {
            const auto state = Gamepads::state(id);
            if (!state) {
                continue;
            }
            const GamepadFrame *previous = gamepad(id);
            const u32 before = previous != nullptr ? previous->state.buttons : 0u;
            next.push_back(GamepadFrame{id, *state, state->buttons & ~before, before & ~state->buttons});
        }
        gamepad_frames_ = std::move(next);
    }

    const InputState::GamepadFrame *InputState::gamepad(u32 id) const noexcept {
        for (const GamepadFrame &frame : gamepad_frames_) {
            if (frame.id == id) {
                return &frame;
            }
        }
        return nullptr;
    }

    bool InputState::gamepad_button_down(u32 id, Gamepads::Button button) const noexcept {
        const GamepadFrame *frame = gamepad(id);
        return frame != nullptr && frame->state.down(button);
    }

    bool InputState::gamepad_button_just_pressed(u32 id, Gamepads::Button button) const noexcept {
        const GamepadFrame *frame = gamepad(id);
        return frame != nullptr && ((frame->pressed_this_tick >> static_cast<u32>(button)) & 1u) != 0;
    }

    bool InputState::gamepad_button_just_released(u32 id, Gamepads::Button button) const noexcept {
        const GamepadFrame *frame = gamepad(id);
        return frame != nullptr && ((frame->released_this_tick >> static_cast<u32>(button)) & 1u) != 0;
    }

    f32 InputState::gamepad_axis(u32 id, Gamepads::Axis axis) const noexcept {
        const GamepadFrame *frame = gamepad(id);
        return frame != nullptr ? frame->state.axis(axis) : 0.0f;
    }

} // namespace SFT::Engine

