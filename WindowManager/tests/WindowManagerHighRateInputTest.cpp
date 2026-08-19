

#include <WindowManager/WindowManager.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {

    using SFT::WindowManager::CursorIcon;
    using SFT::WindowManager::ManagedWindowEvents;
    using SFT::WindowManager::NativeWindowHandle;
    using SFT::WindowManager::Window;
    using SFT::WindowManager::WindowBackendKind;
    using SFT::WindowManager::WindowConfig;
    using SFT::WindowManager::WindowEffect;
    using SFT::WindowManager::WindowEffectResult;
    using SFT::WindowManager::WindowError;
    using SFT::WindowManager::WindowErrorCode;
    using SFT::WindowManager::WindowEvent;
    using SFT::WindowManager::WindowEventKind;
    using SFT::WindowManager::WindowEventPumpMode;
    using SFT::WindowManager::WindowExtent;
    using SFT::WindowManager::WindowId;
    using SFT::WindowManager::WindowManager;
    using SFT::WindowManager::WindowManagerPolicy;
    using SFT::WindowManager::WindowMode;
    using SFT::WindowManager::WindowMouseMoveEvent;
    using SFT::WindowManager::WindowPosition;
    using SFT::WindowManager::WindowResize;
    using SFT::WindowManager::WindowingSystem;
    using SFT::f32;
    using SFT::i64;
    using SFT::u64;
    using SFT::usize;

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    /// Returns the current or globally available steady now ns value.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] u64 steady_now_ns() noexcept {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
    }


    class SyntheticDeviceWindow final : public Window {
      public:


        static usize next_target_sample_count;


        static u32 next_channel_tag;

        /// Destroys the `SyntheticDeviceWindow` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~SyntheticDeviceWindow() noexcept override = default;

        /// Constructs the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::OutOfMemory`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] static expected<unique_ptr<SyntheticDeviceWindow>, WindowError> construct(
            ConstructorKey key,
            const WindowConfig &           ) noexcept {
            auto *window = new (std::nothrow) SyntheticDeviceWindow(key);
            if (window == nullptr) {
                return unexpected(WindowError{WindowErrorCode::OutOfMemory, "test window allocation failed"});
            }
            window->target_sample_count_ = next_target_sample_count;
            window->channel_tag_ = next_channel_tag;
            window->last_generate_time_ = std::chrono::steady_clock::now();
            return unique_ptr<SyntheticDeviceWindow>{window};
        }

        /// Pumps events using the supplied arguments and current state.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
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


            periods = std::min<i64>(periods, max_samples_per_call);
            periods = std::min<i64>(periods, static_cast<i64>(target_sample_count_ - generated_count_));


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

        /// Polls event for available work or state changes.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowEvent> poll_event() noexcept override {
            if (events_.empty()) {
                return std::nullopt;
            }
            WindowEvent event = events_.front();
            events_.pop_front();
            return event;
        }

        /// Returns the generated count for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the current generated count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize generated_count() const noexcept { return generated_count_; }

        /// Returns the current or globally available backend kind value.
        ///
        /// @return Returns the current backend kind value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowBackendKind backend_kind() const noexcept override { return WindowBackendKind::SDL3; }
        /// Returns the runtime or backend type represented by `SyntheticDeviceWindow`.
        ///
        /// @return Returns the current type value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowingSystem type() const noexcept override { return WindowingSystem::SDL3; }
        /// Returns the native backend handle associated with this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<void *, WindowError> native_backend_handle() const noexcept override { return nullptr; }
        /// Returns the native window handle associated with this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<NativeWindowHandle, WindowError> native_window_handle() const noexcept override { return NativeWindowHandle{}; }

        /// Closes requested using the supplied arguments and current state.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool close_requested() const noexcept override { return close_requested_; }
        /// Requests close using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void request_close() noexcept override { close_requested_ = true; }
        /// Changes the logical size to the requested value, creating or removing elements as needed.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool resized() const noexcept override { return false; }
        /// Returns the current or globally available consume resize value.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowResize> consume_resize() noexcept override { return std::nullopt; }

        /// Returns the current or globally available show value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> show() noexcept override { return {}; }
        /// Returns the current or globally available hide value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> hide() noexcept override { return {}; }
        /// Returns the current or globally available focus value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> focus() noexcept override { return {}; }
        /// Returns the current or globally available raise value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> raise() noexcept override { return {}; }
        /// Returns the current or globally available maximize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> maximize() noexcept override { return {}; }
        /// Returns the current or globally available minimize value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> minimize() noexcept override { return {}; }
        /// Returns the current or globally available restore value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> restore() noexcept override { return {}; }
        /// Sets the title for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_title(const char *          ) noexcept override { return {}; }
        /// Returns the current or globally available position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> position() const noexcept override { return WindowPosition{}; }
        /// Sets the position for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_position(WindowPosition             ) noexcept override { return {}; }
        /// Returns the current or globally available global cursor position value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowPosition, WindowError> global_cursor_position() const noexcept override { return WindowPosition{}; }
        /// Returns the size for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> size() const noexcept override { return WindowExtent{640, 480}; }
        /// Sets the size for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_size(WindowExtent           ) noexcept override { return {}; }
        /// Returns the framebuffer size for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<WindowExtent, WindowError> framebuffer_size() const noexcept override { return WindowExtent{640, 480}; }
        /// Sets the minimum size for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_minimum_size(WindowExtent           ) noexcept override { return {}; }
        /// Sets the maximum size for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_maximum_size(WindowExtent           ) noexcept override { return {}; }
        /// Sets the resizable for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_resizable(bool            ) noexcept override { return {}; }
        /// Sets the decorated for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_decorated(bool            ) noexcept override { return {}; }
        /// Sets the fullscreen for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_fullscreen(WindowMode         ) noexcept override { return {}; }
        /// Returns the current or globally available fullscreen mode value.
        ///
        /// @return Returns the current fullscreen mode value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowMode fullscreen_mode() const noexcept override { return WindowMode::Windowed; }
        /// Sets the opacity for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_opacity(f32            ) noexcept override { return {}; }
        /// Returns the current or globally available opacity value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<f32, WindowError> opacity() const noexcept override { return 1.0F; }
        /// Sets the cursor icon for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_icon(CursorIcon         ) noexcept override { return {}; }
        /// Sets the cursor visible for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_visible(bool            ) noexcept override { return {}; }
        /// Sets the cursor grabbed for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_cursor_grabbed(bool            ) noexcept override { return {}; }
        /// Sets the relative mouse mode for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_relative_mouse_mode(bool            ) noexcept override { return {}; }
        /// Sets the mouse locked for this `SyntheticDeviceWindow`.
        ///
        /// @param locked `locked` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_mouse_locked(bool locked) noexcept override {
            mouse_locked_ = locked;
            return {};
        }
        /// Returns the current or globally available mouse locked value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool mouse_locked() const noexcept override { return mouse_locked_; }
        /// Enables window effect using the supplied arguments and current state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowEffectResult enable_window_effect(WindowEffect           ) noexcept override {
            return WindowEffectResult::success();
        }
        /// Sets the effect for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_effect(WindowEffect           ) noexcept override { return {}; }
        /// Sets the blur enabled for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_blur_enabled(bool            ) noexcept override { return {}; }
        /// Sets the transparent for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_transparent(bool            ) noexcept override { return {}; }
        /// Returns the current or globally available required vulkan instance extensions value.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] expected<vector<const char *>, WindowError> required_vulkan_instance_extensions() const noexcept override {
            return vector<const char *>{};
        }
        /// Creates a vulkan surface from the supplied parameters.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> create_vulkan_surface(
            void *             ,
            const void *                         ,
            void *                ) const noexcept override {
            return {};
        }
        /// Returns the current or globally available clipboard text value.
        ///
        /// @return Returns the current clipboard text value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::string clipboard_text() const noexcept override { return {}; }
        /// Sets the clipboard text for this `SyntheticDeviceWindow`.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        expected<void, WindowError> set_clipboard_text(std::string_view         ) noexcept override { return {}; }

      private:
        /// Constructs a `SyntheticDeviceWindow` from the supplied initialization values.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @note This function does not throw exceptions.
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

    bool high_rate_mouse_input_latency_is_bounded() {
        bool passed = true;

        constexpr usize target_sample_count = 4000;
        constexpr auto generation_window = std::chrono::milliseconds(650);
        constexpr auto final_drain_window = std::chrono::milliseconds(120);
        constexpr auto pump_interval = std::chrono::milliseconds(12);
        constexpr u64 max_latency_ceiling_ns = 250ULL * 1'000'000ULL;

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
                        freshest_timestamp_ns = event.timestamp_ns;
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


        passed &= check(freshest.max_ns < max_latency_ceiling_ns,
                        "max observed freshest-sample input latency exceeded the sanity ceiling");

        return passed;
    }


    /// Computes the multiple windows pump concurrently without cross talk math operation over the supplied values or element range.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool multiple_windows_pump_concurrently_without_cross_talk() {
        bool passed = true;

        constexpr usize window_count = 3;
        constexpr usize target_sample_count = 2000;
        constexpr auto generation_window = std::chrono::milliseconds(400);
        constexpr auto final_drain_window = std::chrono::milliseconds(100);
        constexpr auto pump_interval = std::chrono::milliseconds(10);

        WindowManagerPolicy policy{};
        policy.event_pump_mode = WindowEventPumpMode::DedicatedEventThread;
        policy.platform_allows_threads = true;

        WindowManager manager{policy};

        std::vector<std::pair<WindowId, u32>> windows;
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

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    bool passed = high_rate_mouse_input_latency_is_bounded();
    passed &= multiple_windows_pump_concurrently_without_cross_talk();
    return passed ? 0 : 1;
}
