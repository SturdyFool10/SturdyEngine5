// WindowManager's event accumulator: mouse-motion coalescing and the per-window accumulation cap
// (see WindowManagerPolicy's own doc comments). Driven entirely through a fake Window, so this is a
// provider-independent test of the manager itself — the same accumulator both SDL3 and the optional
// GLFW provider feed, which is why it lives here rather than beside either one.
//
// CallerThread pump mode throughout: pump() is then fully synchronous (poll-and-drain inline), so
// every assertion below is deterministic rather than racing a background poll pass.

#include <Platform/Window/WindowManager.hpp>

#include <deque>
#include <iostream>
#include <new>
#include <string>
#include <vector>

namespace {

    using namespace SFT::Platform::Windowing;
    using SFT::f32;
    using SFT::usize;

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    // Replays a fixed script of events on its first pump_events() call and nothing afterwards, so a
    // single pump() call sees exactly the sequence a test set up.
    class ScriptedWindow final : public Window {
      public:
        // Consumed (and cleared) by the next construct() call — Window's factory contract gives no
        // route to pass test data through WindowConfig, and the alternative (a per-instance setter)
        // is unreachable because WindowManager owns every Window it creates.
        static std::vector<WindowEvent> next_script;

        ~ScriptedWindow() noexcept override = default;

        [[nodiscard]] static expected<unique_ptr<ScriptedWindow>, WindowError> construct(
            ConstructorKey key,
            const WindowConfig & /*config*/) noexcept {
            auto *window = new (std::nothrow) ScriptedWindow(key);
            if (window == nullptr) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "test window allocation failed"});
            }
            window->script_ = next_script;
            return unique_ptr<ScriptedWindow>{window};
        }

        expected<void, WindowError> pump_events() noexcept override {
            if (!script_replayed_) {
                events_.insert(events_.end(), script_.begin(), script_.end());
                script_replayed_ = true;
            }
            return {};
        }

        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override {
            if (events_.empty()) {
                return std::nullopt;
            }
            WindowEvent event = events_.front();
            events_.pop_front();
            return event;
        }

        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override { return WindowBackendKind::SDL3; }
        [[nodiscard]] WindowingSystem type() const noexcept override { return WindowingSystem::SDL3; }
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override { return nullptr; }
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override { return NativeWindowHandle{}; }

        [[nodiscard]] bool close_requested() const noexcept override { return close_requested_; }
        void request_close() noexcept override { close_requested_ = true; }
        [[nodiscard]] bool resized() const noexcept override { return false; }
        [[nodiscard]] optional<WindowResize> consume_resize() noexcept override { return std::nullopt; }

        expected<void, WindowError> show() noexcept override { return {}; }
        expected<void, WindowError> hide() noexcept override { return {}; }
        expected<void, WindowError> focus() noexcept override { return {}; }
        expected<void, WindowError> raise() noexcept override { return {}; }
        expected<void, WindowError> maximize() noexcept override { return {}; }
        expected<void, WindowError> minimize() noexcept override { return {}; }
        expected<void, WindowError> restore() noexcept override { return {}; }
        expected<void, WindowError> set_title(const char * /*title*/) noexcept override { return {}; }
        [[nodiscard]] expected<WindowPosition, WindowError> position() const noexcept override { return WindowPosition{}; }
        expected<void, WindowError> set_position(WindowPosition /*position*/) noexcept override { return {}; }
        [[nodiscard]] expected<WindowPosition, WindowError> global_cursor_position() const noexcept override { return WindowPosition{}; }
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override { return WindowExtent{640, 480}; }
        expected<void, WindowError> set_size(WindowExtent /*extent*/) noexcept override { return {}; }
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override { return WindowExtent{640, 480}; }
        expected<void, WindowError> set_minimum_size(WindowExtent /*extent*/) noexcept override { return {}; }
        expected<void, WindowError> set_maximum_size(WindowExtent /*extent*/) noexcept override { return {}; }
        expected<void, WindowError> set_resizable(bool /*enabled*/) noexcept override { return {}; }
        expected<void, WindowError> set_decorated(bool /*enabled*/) noexcept override { return {}; }
        expected<void, WindowError> set_fullscreen(WindowMode /*mode*/) noexcept override { return {}; }
        expected<void, WindowError> set_opacity(f32 /*opacity*/) noexcept override { return {}; }
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override { return 1.0F; }
        expected<void, WindowError> set_cursor_icon(CursorIcon /*icon*/) noexcept override { return {}; }
        expected<void, WindowError> set_cursor_visible(bool /*visible*/) noexcept override { return {}; }
        expected<void, WindowError> set_cursor_grabbed(bool /*grabbed*/) noexcept override { return {}; }
        expected<void, WindowError> set_relative_mouse_mode(bool /*enabled*/) noexcept override { return {}; }
        expected<void, WindowError> set_mouse_locked(bool locked) noexcept override {
            mouse_locked_ = locked;
            return {};
        }
        [[nodiscard]] bool mouse_locked() const noexcept override { return mouse_locked_; }
        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect /*effect*/) noexcept override {
            return WindowEffectResult::success();
        }
        expected<void, WindowError> set_effect(WindowEffect /*effect*/) noexcept override { return {}; }
        expected<void, WindowError> set_blur_enabled(bool /*enabled*/) noexcept override { return {}; }
        expected<void, WindowError> set_transparent(bool /*enabled*/) noexcept override { return {}; }
        [[nodiscard]] expected<vector<const char *>, WindowError> required_vulkan_instance_extensions() const noexcept override {
            return vector<const char *>{};
        }
        expected<void, WindowError> create_vulkan_surface(
            void * /*instance*/,
            const void * /*allocation_callbacks*/,
            void * /*surface_out*/) const noexcept override {
            return {};
        }
        [[nodiscard]] std::string clipboard_text() const noexcept override { return {}; }
        expected<void, WindowError> set_clipboard_text(std::string_view /*text*/) noexcept override { return {}; }

      private:
        explicit ScriptedWindow(ConstructorKey key) noexcept : Window(key) {}

        std::vector<WindowEvent> script_;
        std::deque<WindowEvent> events_;
        bool script_replayed_ = false;
        bool close_requested_ = false;
        bool mouse_locked_ = false;
    };

    std::vector<WindowEvent> ScriptedWindow::next_script;

    [[nodiscard]] WindowEvent motion(f32 x, f32 y, f32 delta_x, f32 delta_y) {
        WindowEvent event{WindowEventKind::MouseMoved};
        event.mouse_move = WindowMouseMoveEvent{x, y, delta_x, delta_y, 0};
        return event;
    }

    // Runs one synchronous pump() over `script` and hands back what the accumulator produced.
    [[nodiscard]] vector<WindowEvent> pump_script(std::vector<WindowEvent> script, WindowManagerPolicy policy, bool &passed) {
        policy.event_pump_mode = WindowEventPumpMode::CallerThread;
        policy.platform_allows_threads = true;
        ScriptedWindow::next_script = std::move(script);

        WindowManager manager{policy};
        const auto spawned = manager.spawn_window<ScriptedWindow>(WindowConfig{});
        passed &= check(spawned.has_value(), "scripted window could not be spawned");
        if (!spawned) {
            return {};
        }

        vector<ManagedWindowEvents> packets;
        passed &= check(manager.pump(packets).has_value(), "WindowManager::pump failed");
        passed &= check(packets.size() == 1, "pump did not return exactly one window packet");
        vector<WindowEvent> events;
        if (packets.size() == 1) {
            events = packets[0].events;
        }
        manager.destroy_window(*spawned);
        ScriptedWindow::next_script.clear();
        return events;
    }

    // A high-polling-rate mouse delivers a long run of MouseMoved events per frame. Each run must
    // collapse to a single event carrying the newest position and the *summed* motion, and must not
    // merge across an intervening event — merging across a button press would move the pointer
    // position that click is attributed to.
    bool mouse_motion_coalesces_without_losing_motion() {
        bool passed = true;

        WindowEvent press{WindowEventKind::MouseButtonPressed};
        press.mouse_button = WindowMouseButtonEvent{
            .button = 1, .clicks = 1, .x = 3.0F, .y = 3.0F, .button_code = MouseButton::Left};
        const std::vector<WindowEvent> script{
            motion(1.0F, 1.0F, 1.0F, 1.0F),
            motion(2.0F, 2.0F, 1.0F, 1.0F),
            motion(3.0F, 3.0F, 1.0F, 1.0F),
            press,
            motion(4.0F, 6.0F, 1.0F, 3.0F),
            motion(5.0F, 9.0F, 1.0F, 3.0F),
        };

        const vector<WindowEvent> coalesced = pump_script(script, WindowManagerPolicy{}, passed);
        passed &= check(coalesced.size() == 3, "adjacent mouse motion was not coalesced to one event per run");
        if (coalesced.size() == 3) {
            passed &= check(coalesced[0].kind == WindowEventKind::MouseMoved &&
                                coalesced[1].kind == WindowEventKind::MouseButtonPressed &&
                                coalesced[2].kind == WindowEventKind::MouseMoved,
                            "coalescing reordered events or merged across a button press");
            passed &= check(coalesced[0].mouse_move.x == 3.0F && coalesced[0].mouse_move.y == 3.0F,
                            "coalesced event did not keep the newest position");
            passed &= check(coalesced[0].mouse_move.delta_x == 3.0F && coalesced[0].mouse_move.delta_y == 3.0F,
                            "coalescing lost motion from the first run");
            passed &= check(coalesced[2].mouse_move.delta_x == 2.0F && coalesced[2].mouse_move.delta_y == 6.0F,
                            "coalescing lost motion from the second run");
        }

        // Opting out must preserve every raw sample point, for a consumer that needs them.
        const vector<WindowEvent> raw = pump_script(script, WindowManagerPolicy{.coalesce_mouse_motion = false}, passed);
        passed &= check(raw.size() == script.size(), "coalesce_mouse_motion=false still merged motion events");
        return passed;
    }

    // The accumulator is bounded even when a consumer stops pumping under a live input stream, and
    // the close/resize latches survive the drop path since they are entry flags, not list entries.
    bool accumulator_is_bounded_and_keeps_state_latches() {
        bool passed = true;
        constexpr usize cap = 8;

        // Alternating kinds so nothing coalesces and the cap is genuinely reached, with a
        // CloseRequested past the cap to prove the latch is not lost with the dropped events.
        std::vector<WindowEvent> script;
        for (usize i = 0; i < cap * 4; ++i) {
            script.push_back(motion(static_cast<f32>(i), 0.0F, 1.0F, 0.0F));
            script.push_back(WindowEvent{WindowEventKind::FocusGained});
        }
        script.push_back(WindowEvent{WindowEventKind::CloseRequested});

        ScriptedWindow::next_script = script;
        WindowManager manager{WindowManagerPolicy{
            .event_pump_mode = WindowEventPumpMode::CallerThread,
            .platform_allows_threads = true,
            .max_accumulated_events_per_window = cap,
        }};
        const auto spawned = manager.spawn_window<ScriptedWindow>(WindowConfig{});
        passed &= check(spawned.has_value(), "scripted window could not be spawned");
        if (spawned) {
            vector<ManagedWindowEvents> packets;
            passed &= check(manager.pump(packets).has_value(), "WindowManager::pump failed");
            if (packets.size() == 1) {
                passed &= check(packets[0].events.size() <= cap, "accumulator grew past its configured cap");
                passed &= check(packets[0].close_requested,
                                "CloseRequested latch was lost among events dropped at the cap");
            }
            manager.destroy_window(*spawned);
        }
        ScriptedWindow::next_script.clear();
        return passed;
    }

} // namespace

int main() {
    bool passed = mouse_motion_coalesces_without_losing_motion();
    passed &= accumulator_is_bounded_and_keeps_state_latches();
    return passed ? 0 : 1;
}
