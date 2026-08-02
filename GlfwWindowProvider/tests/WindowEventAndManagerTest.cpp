#include <Platform/Window/GLFW/Window.hpp>
#include <Platform/Window/SDL3/Window.hpp>
#include <Platform/Window/WindowManager.hpp>

#include <deque>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace SFT::Platform::Windowing;

    struct ThreadTrace {
        std::thread::id constructed;
        std::thread::id pumped;
        std::thread::id polled;
        std::thread::id sampled;
        std::thread::id destroyed;
    };

    ThreadTrace thread_trace;

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    class AffinityWindow final : public Window {
      public:
        ~AffinityWindow() noexcept override { thread_trace.destroyed = std::this_thread::get_id(); }

        [[nodiscard]] static expected<unique_ptr<AffinityWindow>, WindowError> construct(
            ConstructorKey key,
            const WindowConfig & /*config*/) noexcept {
            auto *window = new (std::nothrow) AffinityWindow(key);
            if (window == nullptr) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "test window allocation failed"});
            }
            return unique_ptr<AffinityWindow>{window};
        }

        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override { return WindowBackendKind::SDL3; }
        [[nodiscard]] WindowingSystem type() const noexcept override { return WindowingSystem::SDL3; }
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override { return nullptr; }
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override { return NativeWindowHandle{}; }

        expected<void, WindowError> pump_events() noexcept override {
            thread_trace.pumped = std::this_thread::get_id();
            if (!event_queued_) {
                WindowEvent event{WindowEventKind::MouseButtonPressed};
                event.mouse_button = WindowMouseButtonEvent{
                    .button = 91,
                    .clicks = 2,
                    .x = 12.0F,
                    .y = 34.0F,
                    .button_code = MouseButton::Left,
                };
                events_.push_back(event);
                event_queued_ = true;
            }
            return {};
        }

        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override {
            thread_trace.polled = std::this_thread::get_id();
            if (events_.empty()) {
                return std::nullopt;
            }
            WindowEvent event = events_.front();
            events_.pop_front();
            return event;
        }

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
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override {
            thread_trace.sampled = std::this_thread::get_id();
            return WindowExtent{640, 480};
        }
        expected<void, WindowError> set_minimum_size(WindowExtent /*extent*/) noexcept override { return {}; }
        expected<void, WindowError> set_maximum_size(WindowExtent /*extent*/) noexcept override { return {}; }
        expected<void, WindowError> set_resizable(bool /*enabled*/) noexcept override { return {}; }
        expected<void, WindowError> set_decorated(bool /*enabled*/) noexcept override { return {}; }
        expected<void, WindowError> set_fullscreen(WindowMode /*mode*/) noexcept override { return {}; }
        expected<void, WindowError> set_opacity(f32 /*opacity*/) noexcept override { return {}; }
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override { return 1.0F; }
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
        explicit AffinityWindow(ConstructorKey key) noexcept : Window(key) {
            thread_trace.constructed = std::this_thread::get_id();
        }

        std::deque<WindowEvent> events_;
        bool event_queued_ = false;
        bool close_requested_ = false;
        bool mouse_locked_ = false;
    };

    bool normalization_is_provider_independent() {
        using SFT::Platform::Windowing::GLFW::Detail::normalize_mouse_button;

        bool passed = true;
        passed &= check(normalize_mouse_button(GLFW_MOUSE_BUTTON_LEFT) == MouseButton::Left,
                        "GLFW left button was not normalized");
        passed &= check(normalize_mouse_button(GLFW_MOUSE_BUTTON_MIDDLE) == MouseButton::Middle,
                        "GLFW middle button was not normalized");
        passed &= check(normalize_mouse_button(GLFW_MOUSE_BUTTON_RIGHT) == MouseButton::Right,
                        "GLFW right button was not normalized");
        passed &= check(SDL3::Detail::normalize_mouse_button(SDL_BUTTON_LEFT) == MouseButton::Left,
                        "SDL left button was not normalized");
        passed &= check(SDL3::Detail::normalize_mouse_button(SDL_BUTTON_MIDDLE) == MouseButton::Middle,
                        "SDL middle button was not normalized");
        passed &= check(SDL3::Detail::normalize_mouse_button(SDL_BUTTON_RIGHT) == MouseButton::Right,
                        "SDL right button was not normalized");

        const WindowMouseButtonEvent legacy_aggregate{7, 2, 3.0F, 4.0F};
        passed &= check(legacy_aggregate.button == 7 && legacy_aggregate.clicks == 2,
                        "legacy native mouse-button aggregate fields changed");
        passed &= check(legacy_aggregate.button_code == MouseButton::Unknown,
                        "legacy aggregate did not default the appended normalized field");
        return passed;
    }

    bool manager_keeps_window_access_on_one_owner(WindowManagerPolicy policy, bool expect_dedicated) {
        thread_trace = {};
        const std::thread::id caller = std::this_thread::get_id();
        bool passed = true;

        WindowManager manager{policy};
        passed &= check(manager.has_dedicated_event_thread() == expect_dedicated,
                        "WindowManager selected the wrong event-thread policy");

        const auto spawned = manager.spawn_window<AffinityWindow>(WindowConfig{});
        passed &= check(spawned.has_value(), "fake window could not be spawned");
        if (!spawned) {
            return false;
        }

        vector<ManagedWindowEvents> packets;
        const auto pumped = manager.pump(packets);
        passed &= check(pumped.has_value(), "WindowManager::pump failed");
        passed &= check(packets.size() == 1, "pump did not return exactly one window packet");
        if (packets.size() == 1) {
            passed &= check(packets[0].events.size() == 1, "translated event queue was not drained");
            if (packets[0].events.size() == 1) {
                const WindowMouseButtonEvent &mouse = packets[0].events[0].mouse_button;
                passed &= check(mouse.button == 91, "provider-native button value was not preserved");
                passed &= check(mouse.button_code == MouseButton::Left, "normalized button value was not preserved");
            }
            passed &= check(packets[0].framebuffer_size == optional<WindowExtent>{WindowExtent{640, 480}},
                            "framebuffer state was not sampled");
        }

        const optional<WindowId> primary = manager.primary_window_id();
        passed &= check(primary == optional<WindowId>{*spawned}, "primary window ID was not returned");
        manager.destroy_window(*spawned);

        const std::thread::id owner = thread_trace.constructed;
        passed &= check(owner != std::thread::id{}, "window construction thread was not recorded");
        passed &= check(thread_trace.pumped == owner, "pump_events ran off the window owner thread");
        passed &= check(thread_trace.polled == owner, "poll_event ran off the window owner thread");
        passed &= check(thread_trace.sampled == owner, "framebuffer_size ran off the window owner thread");
        passed &= check(thread_trace.destroyed == owner, "window destruction ran off the window owner thread");
        passed &= check(expect_dedicated ? owner != caller : owner == caller,
                        "window owner thread did not match the selected policy");
        return passed;
    }

} // namespace

int main() {
    bool passed = normalization_is_provider_independent();
    passed &= manager_keeps_window_access_on_one_owner(
        SFT::Platform::Windowing::WindowManagerPolicy{
            .event_pump_mode = SFT::Platform::Windowing::WindowEventPumpMode::CallerThread,
            .platform_allows_threads = true,
        },
        false);
    passed &= manager_keeps_window_access_on_one_owner(
        SFT::Platform::Windowing::WindowManagerPolicy{
            .event_pump_mode = SFT::Platform::Windowing::WindowEventPumpMode::DedicatedEventThread,
            .platform_allows_threads = true,
        },
        SFT::Platform::Windowing::compile_time_window_thread_allowed);
    return passed ? 0 : 1;
}
