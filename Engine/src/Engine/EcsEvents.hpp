#pragma once

#include <Ecs/src/Event.hpp>
#include <Platform/Platform.hpp>

#include <utility>
#include <vector>

namespace SFT::Engine {

    using KeyboardKey = Platform::Windowing::KeyboardKey;

    enum class ButtonAction : u8 {
        Pressed,
        Released,
    };


    struct WindowEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowEvent event{};
    };

    struct KeyboardEvent {
        Platform::Windowing::WindowId window{};
        i32 key = 0;
        i32 scancode = 0;
        u32 modifiers = 0;
        KeyboardKey key_code = KeyboardKey::Unknown;
        ButtonAction action = ButtonAction::Pressed;
        bool repeat = false;


        u64 timestamp_ns = 0;

        /// Returns the current or globally available pressed value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool pressed() const noexcept;
        /// Returns the current or globally available released value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool released() const noexcept;
    };

    struct TextInputEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowTextInputEvent text{};
        u64 timestamp_ns = 0;
    };


    struct TextEditingEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowTextEditingEvent text{};
        u64 timestamp_ns = 0;
    };

    struct MouseMoveEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowMouseMoveEvent mouse{};
        u64 timestamp_ns = 0;
    };

    struct MouseButtonEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowMouseButtonEvent mouse{};
        ButtonAction action = ButtonAction::Pressed;
        u64 timestamp_ns = 0;
    };

    struct MouseWheelEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowMouseWheelEvent wheel{};
        u64 timestamp_ns = 0;
    };

    struct WindowStateEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowEventKind kind = Platform::Windowing::WindowEventKind::CloseRequested;
        Platform::Windowing::WindowPosition position{};
        Platform::Windowing::WindowResize resize{};
        u64 timestamp_ns = 0;
    };


    class PlatformEventInbox {
      public:
        /// Adds the supplied value to the end or work queue.
        ///
        /// @param window Window used or affected by the operation.
        /// @param event Event used or affected by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void push(Platform::Windowing::WindowId window, Platform::Windowing::WindowEvent event);

        /// Drains the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @return Returns the current drain value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::vector<WindowEvent> drain() noexcept;

        /// Reports whether this `PlatformEventInbox` contains no elements or payload.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool empty() const noexcept;

      private:
        std::vector<WindowEvent> pending_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::PlatformEventInbox, "sturdy.engine.platform_event_inbox");
SFT_ECS_EVENT(SFT::Engine::WindowEvent, "sturdy.engine.window_event");
SFT_ECS_EVENT(SFT::Engine::KeyboardEvent, "sturdy.engine.keyboard_event");
SFT_ECS_EVENT(SFT::Engine::TextInputEvent, "sturdy.engine.text_input_event");
SFT_ECS_EVENT(SFT::Engine::TextEditingEvent, "sturdy.engine.text_editing_event");
SFT_ECS_EVENT(SFT::Engine::MouseMoveEvent, "sturdy.engine.mouse_move_event");
SFT_ECS_EVENT(SFT::Engine::MouseButtonEvent, "sturdy.engine.mouse_button_event");
SFT_ECS_EVENT(SFT::Engine::MouseWheelEvent, "sturdy.engine.mouse_wheel_event");
SFT_ECS_EVENT(SFT::Engine::WindowStateEvent, "sturdy.engine.window_state_event");
