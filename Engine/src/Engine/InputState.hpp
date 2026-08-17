#pragma once

#include "EcsEvents.hpp"

#include <Ecs/src/Resource.hpp>
#include <Platform/Platform.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Engine {


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
        [[nodiscard]] Platform::Windowing::KeyModifiers modifiers() const noexcept;

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
        [[nodiscard]] bool mouse_down(Platform::Windowing::MouseButton button) const noexcept;
        /// Performs the mouse just pressed operation for `InputState` using the supplied arguments.
        ///
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_just_pressed(Platform::Windowing::MouseButton button) const noexcept;
        /// Performs the mouse just released operation for `InputState` using the supplied arguments.
        ///
        /// @param button `button` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_just_released(Platform::Windowing::MouseButton button) const noexcept;

        /// Returns the current or globally available mouse x value.
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

      private:
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
        [[nodiscard]] static bool get(const std::vector<bool> &bits, Platform::Windowing::MouseButton button) noexcept;


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
