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

    // Every translated platform event, preserved losslessly for consumers that need an event kind not
    // covered by one of the typed convenience streams below.
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
        // Monotonic capture time (steady_clock epoch, ns) inherited from the originating
        // Platform::Windowing::WindowEvent -- see that struct's doc comment for how each backend
        // populates it.
        u64 timestamp_ns = 0;

        [[nodiscard]] bool pressed() const noexcept { return action == ButtonAction::Pressed; }
        [[nodiscard]] bool released() const noexcept { return action == ButtonAction::Released; }
    };

    struct TextInputEvent {
        Platform::Windowing::WindowId window{};
        Platform::Windowing::WindowTextInputEvent text{};
        u64 timestamp_ns = 0;
    };

    // IME composition/preedit update — see Platform::Windowing::WindowTextEditingEvent's own doc
    // comment, in particular that an empty `text.utf8` means composition just ended.
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

    // Ordinary resource populated by Application's platform pump. It deliberately is not Events<T>:
    // Schedule::run() clears event resources before producer systems execute, so this inbox survives
    // until the built-in producer system publishes its contents into the typed event streams.
    class PlatformEventInbox {
      public:
        void push(Platform::Windowing::WindowId window, Platform::Windowing::WindowEvent event) {
            pending_.push_back(WindowEvent{.window = window, .event = std::move(event)});
        }

        [[nodiscard]] std::vector<WindowEvent> drain() noexcept {
            std::vector<WindowEvent> result;
            result.swap(pending_);
            // Preserve this tick's high-water mark for pending_ rather than leaving it at the
            // zero capacity a default-constructed vector swaps in -- a sustained high-polling-rate
            // mouse would otherwise force a reallocation-from-empty cascade on every single tick's
            // worth of push() calls (WindowManager::pump() has the same fix, same reasoning).
            pending_.reserve(result.capacity());
            return result;
        }

        [[nodiscard]] bool empty() const noexcept { return pending_.empty(); }

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
