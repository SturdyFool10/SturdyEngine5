/// Engine::inject_* events must take the same path as platform events: they reach the ECS event
/// channels and the UI pointer state on the next update.

#include <Async/Scheduler.hpp>
#include <Engine/ScreenUi.hpp>
#include <Engine/EngineModule.hpp>

#include <iostream>

namespace {
    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    struct Seen {
        int key_presses = 0;
        int key_releases = 0;
        int text_events = 0;
        std::string text;
        int wheel_events = 0;
    };
    Seen seen;
} // namespace

int main() {
    using namespace SFT;
    Engine::Engine engine;
    const WindowManager::WindowId window{1};

    // The on-screen UI is application code; it drives itself from the same events.
    Engine::ScreenUi screen_ui{engine};

    engine.update_schedule().add_system([](Ecs::EventReader<Engine::KeyboardEvent> keys,
                                           Ecs::EventReader<Engine::TextInputEvent> text,
                                           Ecs::EventReader<Engine::MouseWheelEvent> wheel) noexcept {
        for (const Engine::KeyboardEvent &event : keys.read()) {
            (event.action == Engine::ButtonAction::Pressed ? seen.key_presses : seen.key_releases) += 1;
            if (event.key_code != WindowManager::KeyboardKey::Space) {
                seen.key_presses += 100;
            }
        }
        for (const Engine::TextInputEvent &event : text.read()) {
            ++seen.text_events;
            seen.text += event.text.utf8;
        }
        for ([[maybe_unused]] const Engine::MouseWheelEvent &event : wheel.read()) {
            ++seen.wheel_events;
        }
    });

    engine.inject_key_event(window, WindowManager::KeyboardKey::Space, true);
    engine.inject_key_event(window, WindowManager::KeyboardKey::Space, false);
    engine.inject_text_event(window, "héllo");
    engine.inject_mouse_move(window, 120.0f, 45.0f);
    engine.inject_mouse_button(window, WindowManager::MouseButton::Left, true, 120.0f, 45.0f);
    engine.inject_mouse_wheel(window, 0.0f, 1.0f);
    engine.update(1.0 / 60.0);

    check(seen.key_presses == 1 && seen.key_releases == 1, "an injected key press and release must reach KeyboardEvent readers with the right key");
    check(seen.text_events == 1 && seen.text == "héllo", "injected text must arrive intact through TextInputEvent");
    check(seen.wheel_events == 1, "an injected wheel event must reach MouseWheelEvent readers");
    const auto &pointer = screen_ui.input().pointer();
    check(pointer.position.x == 120.0f && pointer.position.y == 45.0f, "an injected pointer move must update the UI pointer position");
    check(pointer.down, "an injected left press must set the UI pointer down");

    // Text longer than one event's buffer is split, not truncated, and never mid-character.
    seen = {};
    std::string long_text;
    for (int i = 0; i < 60; ++i) {
        long_text += "é€";
    }
    engine.inject_text_event(window, long_text);
    engine.update(1.0 / 60.0);
    check(seen.text_events > 1, "over-long injected text must be split across events");
    check(seen.text == long_text, "split text must reassemble to the original");
    Async::Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
