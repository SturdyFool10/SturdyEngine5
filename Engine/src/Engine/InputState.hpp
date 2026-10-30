#pragma once

#include <Engine/EcsEvents.hpp>

#include <Ecs/Resource.hpp>
#include <WindowManager/WindowManager.hpp>

#include <WindowManager/Gamepad.hpp>

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Engine {

    namespace Gamepads = WindowManager::Gamepads;


    class InputState {
      public:


        /// Performs the begin tick operation for `InputState` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void begin_tick() noexcept;

        /// Applies the supplied operation or state to `InputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const KeyboardEvent &event) noexcept;

        /// Applies the supplied operation or state to `InputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const TextInputEvent &event) noexcept;

        /// Applies the supplied operation or state to `InputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const TextEditingEvent &event) noexcept;

        /// Applies the supplied operation or state to `InputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const MouseMoveEvent &event) noexcept;

        /// Applies the supplied operation or state to `InputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const MouseButtonEvent &event) noexcept;

        /// Applies the supplied operation or state to `InputState`.
        ///
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void apply(const MouseWheelEvent &event) noexcept;

        /// Performs the key down operation for `InputState` using the supplied arguments.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool key_down(KeyboardKey key) const noexcept;
        /// Performs the key just pressed operation for `InputState` using the supplied arguments.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool key_just_pressed(KeyboardKey key) const noexcept;
        /// Performs the key just released operation for `InputState` using the supplied arguments.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool key_just_released(KeyboardKey key) const noexcept;

        /// Returns the current or globally available modifiers value.
        ///
        /// @return Returns the current modifiers value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowManager::KeyModifiers modifiers() const noexcept;

        /// Returns the current or globally available text this tick value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string_view text_this_tick() const noexcept;


        /// Returns the current or globally available composition text value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string_view composition_text() const noexcept;
        /// Returns the current or globally available composing value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool composing() const noexcept;

        /// Performs the mouse down operation for `InputState` using the supplied arguments.
        ///
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_down(WindowManager::MouseButton button) const noexcept;
        /// Performs the mouse just pressed operation for `InputState` using the supplied arguments.
        ///
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_just_pressed(WindowManager::MouseButton button) const noexcept;
        /// Performs the mouse just released operation for `InputState` using the supplied arguments.
        ///
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_just_released(WindowManager::MouseButton button) const noexcept;

        /// Returns the current or globally available mouse x value, in window coordinates. Multiply by
        /// `WindowSnapshot::pixel_density()` (or use `window_to_framebuffer`) before comparing against
        /// framebuffer pixels, e.g. for `Camera::screen_ray`.
        ///
        /// @return Returns the current mouse x value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 mouse_x() const noexcept;
        /// Returns the current or globally available mouse y value.
        ///
        /// @return Returns the current mouse y value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 mouse_y() const noexcept;


        /// Returns the current or globally available mouse delta x value.
        ///
        /// @return Returns the current mouse delta x value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 mouse_delta_x() const noexcept;
        /// Returns the current or globally available mouse delta y value.
        ///
        /// @return Returns the current mouse delta y value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 mouse_delta_y() const noexcept;
        /// Returns the current or globally available wheel delta x value.
        ///
        /// @return Returns the current wheel delta x value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 wheel_delta_x() const noexcept;
        /// Returns the current or globally available wheel delta y value.
        ///
        /// @return Returns the current wheel delta y value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] f32 wheel_delta_y() const noexcept;

        // -- Touch, pen and gamepads --

        /// A finger. A lifted finger stays listed (with `just_released`) for the tick it lifted in, then disappears.
        struct TouchPoint {
            WindowManager::WindowId window{};
            u64 device = 0;
            u64 finger = 0;
            /// Window coordinates.
            f32 x = 0.0f;
            f32 y = 0.0f;
            /// Movement since the previous tick.
            f32 dx = 0.0f;
            f32 dy = 0.0f;
            f32 pressure = 0.0f;
            bool down = false;
            bool just_pressed = false;
            bool just_released = false;
            /// The system took the touch away (a palm, a gesture): treat it as released without activating anything.
            bool cancelled = false;
        };

        /// A pen. It stays listed while in range, and for the tick it left range in.
        struct PenPoint {
            WindowManager::WindowId window{};
            u32 pen = 0;
            f32 x = 0.0f;
            f32 y = 0.0f;
            bool in_range = false;
            /// The tip is on the surface.
            bool down = false;
            bool eraser = false;
            bool just_down = false;
            bool just_up = false;
            /// Pressure 0..1, tilt in degrees, distance 0..1, rotation in degrees, slider 0..1, barrel pressure 0..1.
            f32 pressure = 0.0f;
            f32 x_tilt = 0.0f;
            f32 y_tilt = 0.0f;
            f32 distance = 0.0f;
            f32 rotation = 0.0f;
            f32 slider = 0.0f;
            f32 tangential_pressure = 0.0f;
            /// Bit `n` is barrel button `n + 1`.
            u32 buttons = 0;
        };

        /// One mapped gamepad as it is this tick. The masks hold the buttons that changed since last tick.
        struct GamepadFrame {
            u32 id = 0;
            Gamepads::State state{};
            u32 pressed_this_tick = 0;
            u32 released_this_tick = 0;
        };

        void apply(const TouchEvent &event) noexcept;
        void apply(const PenEvent &event) noexcept;

        [[nodiscard]] std::span<const TouchPoint> touches() const noexcept { return touches_; }
        [[nodiscard]] std::span<const PenPoint> pens() const noexcept { return pens_; }

        /// Gamepads are sampled once per tick, only after this is turned on: they start the joystick subsystem.
        void set_gamepads_enabled(bool enabled) noexcept { gamepads_enabled_ = enabled; }
        [[nodiscard]] bool gamepads_enabled() const noexcept { return gamepads_enabled_; }
        /// Samples every mapped gamepad (called by the input system each tick).
        void poll_gamepads() noexcept;
        [[nodiscard]] std::span<const GamepadFrame> gamepads() const noexcept { return gamepad_frames_; }
        /// Devices plugged or unplugged since the previous tick.
        [[nodiscard]] std::span<const Gamepads::Event> gamepad_events() const noexcept { return gamepad_events_; }
        /// The frame of gamepad `id`, or null when it is not connected (or not a mapped gamepad).
        [[nodiscard]] const GamepadFrame *gamepad(u32 id) const noexcept;
        [[nodiscard]] bool gamepad_button_down(u32 id, Gamepads::Button button) const noexcept;
        [[nodiscard]] bool gamepad_button_just_pressed(u32 id, Gamepads::Button button) const noexcept;
        [[nodiscard]] bool gamepad_button_just_released(u32 id, Gamepads::Button button) const noexcept;
        [[nodiscard]] f32 gamepad_axis(u32 id, Gamepads::Axis axis) const noexcept;

      private:
        std::vector<TouchPoint> touches_;
        std::vector<PenPoint> pens_;
        bool gamepads_enabled_ = false;
        std::vector<GamepadFrame> gamepad_frames_;
        std::vector<Gamepads::Event> gamepad_events_;

        /// Returns the value or resource currently represented by `InputState`.
        ///
        /// @param bits `bits` value used by the operation.
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static bool get(const std::vector<bool> &bits, KeyboardKey key) noexcept;
        /// Returns the value or resource currently represented by `InputState`.
        ///
        /// @param bits `bits` value used by the operation.
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static bool get(const std::vector<bool> &bits, WindowManager::MouseButton button) noexcept;


        static constexpr usize key_count = 0x407;
        std::vector<bool> key_pressed_ = std::vector<bool>(key_count, false);
        std::vector<bool> key_just_pressed_ = std::vector<bool>(key_count, false);
        std::vector<bool> key_just_released_ = std::vector<bool>(key_count, false);
        std::string text_this_tick_;
        std::string composition_text_;
        bool composing_ = false;

        static constexpr usize mouse_button_count = 16;
        std::vector<bool> mouse_pressed_ = std::vector<bool>(mouse_button_count, false);
        std::vector<bool> mouse_just_pressed_ = std::vector<bool>(mouse_button_count, false);
        std::vector<bool> mouse_just_released_ = std::vector<bool>(mouse_button_count, false);

        f32 mouse_x_ = 0.0f;
        f32 mouse_y_ = 0.0f;
        f32 mouse_delta_x_ = 0.0f;
        f32 mouse_delta_y_ = 0.0f;
        f32 wheel_delta_x_ = 0.0f;
        f32 wheel_delta_y_ = 0.0f;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::InputState, "sturdy.engine.input_state");
