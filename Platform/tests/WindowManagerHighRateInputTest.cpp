// End-to-end stress/latency test for the high-polling-rate-mouse path described in the input-latency
// research writeup this engine's WindowManager was tuned against (see WindowManagerPolicy's own doc
// comments): a synthetic "device" window paces itself against real wall-clock time to emit MouseMoved
// events at 8kHz (one every 125us), and a consumer loop pumps at a realistic ~80Hz cadence, measuring
// how stale each delivered event's timestamp_ns is by the time it's observed. This exercises the real
// DedicatedEventThread poll_loop() background-thread path (falling back to synchronous CallerThread
// wherever compile_time_window_thread_allowed is false, e.g. macOS/Web) without needing any
// platform-specific code in the test itself, since the synthetic device only cares about elapsed wall
// time, not which thread calls pump_events().

#include <Platform/Window/WindowManager.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace SFT::Platform::Windowing;
    using SFT::f32;
    using SFT::i64;
    using SFT::u64;
    using SFT::usize;

    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    [[nodiscard]] u64 steady_now_ns() noexcept {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
    }

    // Generates MouseMoved events at a real 8kHz cadence, paced by wall-clock time rather than a fixed
    // script, so it behaves the same whether pump_events() is called from a background poll_loop()
    // thread (DedicatedEventThread mode) or synchronously from the caller (CallerThread fallback).
    class SyntheticDeviceWindow final : public Window {
      public:
        // Consumed by the next construct() call -- same reasoning as ScriptedWindow::next_script in
        // WindowManagerAccumulatorTest.cpp: Window's factory contract has no other route to pass
        // per-instance test parameters through.
        static usize next_target_sample_count;

        // Also consumed by the next construct() call. Stamped into every generated event's otherwise-
        // unused mouse_move.buttons field so a multi-window test can verify WindowManager never lets
        // one window's events end up attributed to another's ManagedWindowEvents.
        static u32 next_channel_tag;

        ~SyntheticDeviceWindow() noexcept override = default;

        [[nodiscard]] static expected<unique_ptr<SyntheticDeviceWindow>, WindowError> construct(
            ConstructorKey key,
            const WindowConfig & /*config*/) noexcept {
            auto *window = new (std::nothrow) SyntheticDeviceWindow(key);
            if (window == nullptr) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "test window allocation failed"});
            }
            window->target_sample_count_ = next_target_sample_count;
            window->channel_tag_ = next_channel_tag;
            window->last_generate_time_ = std::chrono::steady_clock::now();
            return unique_ptr<SyntheticDeviceWindow>{window};
        }

        expected<void, WindowError> pump_events() noexcept override {
            if (generated_count_ >= target_sample_count_) {
                return {};
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = now - last_generate_time_;
            i64 periods = elapsed / sample_period;
            if (periods <= 0) {
                return {};
            }
            // Cap per-call generation so a debugger pause or a slow CI box can't produce a runaway
            // burst, and never generate past the configured target.
            periods = std::min<i64>(periods, max_samples_per_call);
            periods = std::min<i64>(periods, static_cast<i64>(target_sample_count_ - generated_count_));

            // Advance by exactly the consumed period count (not "now") to avoid accumulating drift
            // across many calls.
            last_generate_time_ += periods * sample_period;

            for (i64 i = 0; i < periods; ++i) {
                WindowEvent event{WindowEventKind::MouseMoved};
                event.timestamp_ns = steady_now_ns();
                event.mouse_move = WindowMouseMoveEvent{
                    static_cast<f32>(generated_count_), 0.0F, 1.0F, 0.0F, channel_tag_,
                };
                events_.push_back(event);
                ++generated_count_;
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

        [[nodiscard]] usize generated_count() const noexcept { return generated_count_; }

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
        explicit SyntheticDeviceWindow(ConstructorKey key) noexcept : Window(key) {}

        static constexpr auto sample_period = std::chrono::nanoseconds(125'000); // 8kHz
        static constexpr i64 max_samples_per_call = 512;

        std::deque<WindowEvent> events_;
        std::chrono::steady_clock::time_point last_generate_time_{};
        usize target_sample_count_ = 0;
        usize generated_count_ = 0;
        u32 channel_tag_ = 0;
        bool close_requested_ = false;
        bool mouse_locked_ = false;
    };

    usize SyntheticDeviceWindow::next_target_sample_count = 0;
    u32 SyntheticDeviceWindow::next_channel_tag = 0;

    // Drives WindowManager with a sustained 8kHz synthetic mouse stream and reports throughput/latency.
    // Latency is measured against WindowEvent::timestamp_ns (steady_clock epoch), so this doubles as a
    // regression test for the per-event-timestamp plumbing added for latency measurement in the first
    // place: coalesce_mouse_motion() must keep the *latest* sample's timestamp (see WindowManager.cpp)
    // or every reported latency here would read as the entire test duration instead of milliseconds.
    bool high_rate_mouse_input_latency_is_bounded() {
        bool passed = true;

        constexpr usize target_sample_count = 4000; // ~500ms of nominal 8kHz sampling
        constexpr auto generation_window = std::chrono::milliseconds(650); // generation + drain slack
        constexpr auto final_drain_window = std::chrono::milliseconds(120);
        constexpr auto pump_interval = std::chrono::milliseconds(12); // ~80Hz consumer cadence
        constexpr u64 max_latency_ceiling_ns = 250ULL * 1'000'000ULL; // 250ms: generous, CI-safe

        SyntheticDeviceWindow::next_target_sample_count = target_sample_count;

        WindowManagerPolicy policy{};
        policy.event_pump_mode = WindowEventPumpMode::DedicatedEventThread;
        policy.platform_allows_threads = true;

        WindowManager manager{policy};
        const auto spawned = manager.spawn_window<SyntheticDeviceWindow>(WindowConfig{});
        passed &= check(spawned.has_value(), "synthetic device window could not be spawned");
        if (!spawned) {
            return passed;
        }

        // Two latency series, deliberately not the same metric: `latencies_ns` covers *every*
        // MouseMoved event a pump() call hands back, `freshest_latencies_ns` covers only the last
        // one per call. Coalescing now only merges runs *within* one drain pass rather than across
        // the whole inter-pump gap (see WindowManager::drain_window_into()'s own doc comment on that
        // relaxation), so a single pump() call after a longer gap can legitimately return several
        // committed events -- earlier ones in that batch were fresh *when committed* but are stale by
        // the time this test's slower consumer cadence finally drains them. What actually matters for
        // a real consumer (e.g. InputState, which takes the latest position and the summed delta) is
        // how stale the *freshest* sample is when observed, which freshest_latencies_ns captures.
        std::vector<u64> latencies_ns;
        std::vector<u64> freshest_latencies_ns;
        usize received_count = 0;
        const auto drain_once = [&]() {
            vector<ManagedWindowEvents> packets;
            passed &= check(manager.pump(packets).has_value(), "WindowManager::pump failed during high-rate stress");
            for (const ManagedWindowEvents &packet : packets) {
                bool have_freshest = false;
                u64 freshest_timestamp_ns = 0;
                for (const WindowEvent &event : packet.events) {
                    if (event.kind == WindowEventKind::MouseMoved) {
                        ++received_count;
                        const u64 now_ns = steady_now_ns();
                        latencies_ns.push_back(event.timestamp_ns <= now_ns ? now_ns - event.timestamp_ns : 0);
                        have_freshest = true;
                        freshest_timestamp_ns = event.timestamp_ns; // events arrive in FIFO order
                    }
                }
                if (have_freshest) {
                    const u64 now_ns = steady_now_ns();
                    freshest_latencies_ns.push_back(
                        freshest_timestamp_ns <= now_ns ? now_ns - freshest_timestamp_ns : 0);
                }
            }
        };

        const auto test_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - test_start < generation_window) {
            drain_once();
            std::this_thread::sleep_for(pump_interval);
        }

        // Generation has stopped (or is stopping) by now; keep draining briefly to collect stragglers
        // still in flight through the accumulator.
        const auto final_drain_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - final_drain_start < final_drain_window) {
            drain_once();
            std::this_thread::sleep_for(pump_interval);
        }

        const usize generated_count = manager.with_window(*spawned,
                                                            [](Window &w) { return static_cast<SyntheticDeviceWindow &>(w).generated_count(); })
                                           .value_or(0);
        manager.destroy_window(*spawned);

        passed &= check(generated_count > 0, "synthetic device generated no samples");
        passed &= check(received_count > 0, "no events were received by the consumer");
        passed &= check(received_count <= generated_count, "received more coalesced events than were generated");

        struct Stats {
            u64 min_ns = 0;
            u64 max_ns = 0;
            double avg_ms = 0.0;
            u64 p99_ns = 0;
        };
        const auto compute_stats = [](std::vector<u64> values) -> Stats {
            if (values.empty()) {
                return {};
            }
            Stats stats;
            stats.min_ns = values.front();
            u64 sum_ns = 0;
            for (u64 value_ns : values) {
                stats.min_ns = std::min(stats.min_ns, value_ns);
                stats.max_ns = std::max(stats.max_ns, value_ns);
                sum_ns += value_ns;
            }
            stats.avg_ms = (static_cast<double>(sum_ns) / static_cast<double>(values.size())) / 1e6;
            std::sort(values.begin(), values.end());
            stats.p99_ns = values[static_cast<usize>(static_cast<double>(values.size() - 1) * 0.99)];
            return stats;
        };

        const Stats all_events = compute_stats(latencies_ns);
        const Stats freshest = compute_stats(freshest_latencies_ns);

        std::cout << "High-rate mouse input benchmark: generated=" << generated_count
                  << " received(coalesced)=" << received_count
                  << " all_events_latency_ms[min=" << (static_cast<double>(all_events.min_ns) / 1e6)
                  << " avg=" << all_events.avg_ms << " p99=" << (static_cast<double>(all_events.p99_ns) / 1e6)
                  << " max=" << (static_cast<double>(all_events.max_ns) / 1e6) << "]"
                  << " freshest_per_pump_latency_ms[min=" << (static_cast<double>(freshest.min_ns) / 1e6)
                  << " avg=" << freshest.avg_ms << " p99=" << (static_cast<double>(freshest.p99_ns) / 1e6)
                  << " max=" << (static_cast<double>(freshest.max_ns) / 1e6) << "]\n";

        // The ceiling is checked against the freshest-per-pump-call series, not the all-events one:
        // that's the metric a real consumer (InputState, which only ever keeps the latest position)
        // actually experiences. An intermediate, already-superseded coalesced chunk sitting in the
        // ring a while before drain is expected and harmless -- see drain_window_into()'s own doc
        // comment on why coalescing runs got shorter with the lock-free ring.
        passed &= check(freshest.max_ns < max_latency_ceiling_ns,
                        "max observed freshest-sample input latency exceeded the sanity ceiling");

        return passed;
    }

    // Multiple windows pumping concurrently at real 8kHz rates through the shared channels_
    // directory -- specifically stresses the part that changed shape the most in the move to a
    // lock-free per-window ring: the directory snapshot-under-lock (spawn_window()/destroy_window()
    // on poll_loop()'s thread, pump()'s consumer-side snapshot) now has more than one live entry
    // being concurrently produced into while it's read. Verifies both throughput (each window
    // receives events) and correctness (no window's events are ever attributed to another window's
    // WindowId -- checked via a per-window tag stamped into mouse_move.buttons).
    bool multiple_windows_pump_concurrently_without_cross_talk() {
        bool passed = true;

        constexpr usize window_count = 3;
        constexpr usize target_sample_count = 2000; // ~250ms of nominal 8kHz sampling, per window
        constexpr auto generation_window = std::chrono::milliseconds(400);
        constexpr auto final_drain_window = std::chrono::milliseconds(100);
        constexpr auto pump_interval = std::chrono::milliseconds(10);

        WindowManagerPolicy policy{};
        policy.event_pump_mode = WindowEventPumpMode::DedicatedEventThread;
        policy.platform_allows_threads = true;

        WindowManager manager{policy};

        std::vector<std::pair<WindowId, u32>> windows; // (id, expected tag)
        for (u32 tag = 1; tag <= window_count; ++tag) {
            SyntheticDeviceWindow::next_target_sample_count = target_sample_count;
            SyntheticDeviceWindow::next_channel_tag = tag;
            const auto spawned = manager.spawn_window<SyntheticDeviceWindow>(WindowConfig{});
            passed &= check(spawned.has_value(), "synthetic device window could not be spawned");
            if (spawned) {
                windows.emplace_back(*spawned, tag);
            }
        }
        passed &= check(windows.size() == window_count, "not all synthetic device windows were spawned");
        if (windows.size() != window_count) {
            return passed;
        }

        std::vector<usize> received_per_window(window_count, 0);
        const auto drain_once = [&]() {
            vector<ManagedWindowEvents> packets;
            passed &= check(manager.pump(packets).has_value(), "WindowManager::pump failed during multi-window stress");
            for (const ManagedWindowEvents &packet : packets) {
                usize window_index = window_count;
                for (usize i = 0; i < windows.size(); ++i) {
                    if (windows[i].first == packet.window_id) {
                        window_index = i;
                        break;
                    }
                }
                passed &= check(window_index != window_count, "pump() returned a packet for an unknown window");
                if (window_index == window_count) {
                    continue;
                }
                const u32 expected_tag = windows[window_index].second;
                for (const WindowEvent &event : packet.events) {
                    if (event.kind != WindowEventKind::MouseMoved) {
                        continue;
                    }
                    ++received_per_window[window_index];
                    passed &= check(event.mouse_move.buttons == expected_tag,
                                    "a window's packet contained another window's event (cross-talk)");
                }
            }
        };

        const auto test_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - test_start < generation_window) {
            drain_once();
            std::this_thread::sleep_for(pump_interval);
        }
        const auto final_drain_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - final_drain_start < final_drain_window) {
            drain_once();
            std::this_thread::sleep_for(pump_interval);
        }

        for (const auto &[id, tag] : windows) {
            manager.destroy_window(id);
        }

        for (usize i = 0; i < window_count; ++i) {
            passed &= check(received_per_window[i] > 0, "a window received no events during concurrent multi-window stress");
        }

        std::cout << "Multi-window stress: " << window_count << " windows, received_per_window=[";
        for (usize i = 0; i < window_count; ++i) {
            std::cout << received_per_window[i] << (i + 1 < window_count ? "," : "");
        }
        std::cout << "]\n";

        return passed;
    }

} // namespace

int main() {
    bool passed = high_rate_mouse_input_latency_is_bounded();
    passed &= multiple_windows_pump_concurrently_without_cross_talk();
    return passed ? 0 : 1;
}
