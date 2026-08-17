#include <Platform/src/Platform/Window/WindowManager.hpp>
#include "WindowManager.hpp"

#include <Async/src/SpscRingBuffer.hpp>

#include <chrono>

#include <tracy/Tracy.hpp>

namespace SFT::Platform::Windowing {


    class WindowEventChannel {
      public:
        /// Constructs a `WindowEventChannel` from the supplied initialization values.
        ///
        /// @param ring_capacity `ring_capacity` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        explicit WindowEventChannel(usize ring_capacity) noexcept : ring(ring_capacity) {}

        Async::SpscRingBuffer<WindowEvent> ring;


        optional<WindowEvent> pending;


        std::atomic<bool> close_requested{false};


        std::atomic<bool> resized{false};

        /// Returns the current or globally available take resized value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool take_resized() noexcept { return resized.exchange(false, std::memory_order_acq_rel); }

        /// Sets the framebuffer size for this `WindowEventChannel`.
        ///
        /// @param size Requested or available size for the operation.
        ///
        /// @note This function does not throw exceptions.
        void set_framebuffer_size(WindowExtent size) noexcept {
            const u64 packed = (static_cast<u64>(size.x) << 32U) | static_cast<u64>(size.y);
            framebuffer_size_packed_.store(packed, std::memory_order_release);
        }

        /// Returns the framebuffer size for this `WindowEventChannel`.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] optional<WindowExtent> framebuffer_size() const noexcept {
            const u64 packed = framebuffer_size_packed_.load(std::memory_order_acquire);
            if (packed == unknown_framebuffer_size) {
                return std::nullopt;
            }
            return WindowExtent{static_cast<u32>(packed >> 32U), static_cast<u32>(packed & 0xFFFFFFFFU)};
        }

      private:
        static constexpr u64 unknown_framebuffer_size = ~u64{0};
        std::atomic<u64> framebuffer_size_packed_{unknown_framebuffer_size};
    };

    namespace {

        using ChannelDirectory = vector<WindowEventChannelEntry>;

        /// Finds channel in the available state.
        ///
        /// @param channels `channels` value used by the operation.
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WindowEventChannel *find_channel(ChannelDirectory &channels, WindowId id) noexcept {
            for (auto &[channel_id, channel] : channels) {
                if (channel_id == id) {
                    return channel.get();
                }
            }
            return nullptr;
        }


        /// Performs the coalesce mouse motion operation for `Windowing` using the supplied arguments.
        ///
        /// @param previous `previous` value used by the operation.
        /// @param incoming `incoming` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool coalesce_mouse_motion(WindowEvent &previous, const WindowEvent &incoming) noexcept {
            if (previous.kind != WindowEventKind::MouseMoved || incoming.kind != WindowEventKind::MouseMoved) {
                return false;
            }
            previous.mouse_move.x = incoming.mouse_move.x;
            previous.mouse_move.y = incoming.mouse_move.y;
            previous.mouse_move.delta_x += incoming.mouse_move.delta_x;
            previous.mouse_move.delta_y += incoming.mouse_move.delta_y;
            previous.mouse_move.buttons |= incoming.mouse_move.buttons;


            previous.timestamp_ns = incoming.timestamp_ns;
            return true;
        }


        /// Performs the materialize channel operation for `Windowing` using the supplied arguments.
        ///
        /// @param id Identifier of the target object or resource.
        /// @param channel `channel` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ManagedWindowEvents materialize_channel(WindowId id, WindowEventChannel &channel) noexcept {
            ManagedWindowEvents collected{.window_id = id, .events = {}, .framebuffer_size = {}};


            collected.events.reserve(16);
            channel.ring.drain_into(collected.events);
            collected.close_requested = channel.close_requested.load(std::memory_order_acquire);
            collected.resized = channel.take_resized();
            collected.framebuffer_size = channel.framebuffer_size();
            return collected;
        }

    } // namespace

    /// Performs the window manager operation for `Windowing` using the supplied arguments.
    ///
    /// @param policy `policy` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    WindowManager::WindowManager(WindowManagerPolicy policy) noexcept
        : policy_(policy) {
        ZoneScopedN("WindowManager::WindowManager");
        if (policy_.event_pump_mode == WindowEventPumpMode::DedicatedEventThread &&
            policy_.platform_allows_threads && compile_time_window_thread_allowed) {
            event_thread_ = std::make_unique<Async::DedicatedThread>("WindowEventThread");
            running_.store(true, std::memory_order_release);


            (void)event_thread_->run([this]() { poll_loop(); });
        }
    }

    /// Destroys the `Windowing` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    WindowManager::~WindowManager() {
        ZoneScopedN("WindowManager::~WindowManager");
        if (event_thread_) {


            running_.store(false, std::memory_order_release);
            wake_poll_loop();
        } else {
            windows_.clear();
        }
    }

    /// Returns the current or globally available policy value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const WindowManagerPolicy &WindowManager::policy() const noexcept { return policy_; }

    /// Reports whether this `Windowing` has dedicated event thread.
    ///
    /// @return Returns the current has dedicated event thread value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool WindowManager::has_dedicated_event_thread() const noexcept { return event_thread_ != nullptr; }

    /// Destroys the window identified by the supplied parameters.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void WindowManager::destroy_window(WindowId id) noexcept {
        ZoneScopedN("WindowManager::destroy_window");
        dispatch([this, id]() {
            std::erase_if(windows_, [id](const unique_ptr<Window> &w) { return w->id() == id; });
            if (primary_window_id_ == id) {
                primary_window_id_ = windows_.empty() ? optional<WindowId>{} : optional<WindowId>{windows_.front()->id()};
            }


            auto guard = channels_.lock();
            std::erase_if(*guard, [id](const WindowEventChannelEntry &entry) { return entry.first == id; });
        });
    }

    /// Performs the seed channel operation for `Windowing` using the supplied arguments.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void WindowManager::seed_channel(WindowId id) noexcept {
        ZoneScopedN("WindowManager::seed_channel");
        auto channel = std::make_shared<WindowEventChannel>(policy_.max_accumulated_events_per_window);
        auto guard = channels_.lock();
        guard->emplace_back(id, std::move(channel));
    }

    /// Returns the current or globally available primary window ID value.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note This function does not throw exceptions.
    [[nodiscard]] optional<WindowId> WindowManager::primary_window_id() const noexcept {
        ZoneScopedN("WindowManager::primary_window_id");
        return dispatch([this]() { return primary_window_id_; });
    }

    /// Returns the window count for this `Windowing`.
    ///
    /// @return Returns the current window count value.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize WindowManager::window_count() noexcept {
        ZoneScopedN("WindowManager::window_count");
        return dispatch([this]() { return windows_.size(); });
    }

    /// Flushes pending.
    ///
    /// @param channel `channel` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void WindowManager::flush_pending(WindowEventChannel &channel) noexcept {
        if (!channel.pending.has_value()) {
            return;
        }
        push_or_warn(channel, *channel.pending);
        channel.pending.reset();
    }

    /// Adds the supplied value to the end or work queue.
    ///
    /// @param channel `channel` value used by the operation.
    /// @param event Event used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void WindowManager::push_or_warn(WindowEventChannel &channel, const WindowEvent &event) noexcept {
        if (channel.ring.try_push(event)) {
            return;
        }


        if (!accumulator_overflow_warned_) {
            accumulator_overflow_warned_ = true;
            Foundation::log_warn(
                "WindowManager: a window's {}-slot ring is full; dropping further events until "
                "pump() drains it. The consumer is not pumping.",
                channel.ring.capacity());
        }
    }

    /// Drains window into using the supplied arguments and current state.
    ///
    /// @param window Window used or affected by the operation.
    /// @param channel `channel` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] usize WindowManager::drain_window_into(Window &window, WindowEventChannel &channel) noexcept {
        ZoneScopedN("WindowManager::drain_window_into");
        if (channel.ring.size() == 0) {


            accumulator_overflow_warned_ = false;
        }
        usize drained = 0;
        while (auto event = window.poll_event()) {
            ++drained;


            if (event->kind == WindowEventKind::CloseRequested) {
                channel.close_requested.store(true, std::memory_order_release);
            } else if (event->kind == WindowEventKind::Resized || event->kind == WindowEventKind::FramebufferResized) {
                channel.resized.store(true, std::memory_order_release);
            }

            if (policy_.coalesce_mouse_motion && channel.pending.has_value() &&
                coalesce_mouse_motion(*channel.pending, *event)) {
                continue;
            }


            flush_pending(channel);

            if (policy_.coalesce_mouse_motion && event->kind == WindowEventKind::MouseMoved) {


                channel.pending = *event;
                continue;
            }

            push_or_warn(channel, *event);
        }
        flush_pending(channel);
        return drained;
    }

    /// Polls loop for available work or state changes.
    ///
    /// @return Returns the current poll loop value.
    /// @note This function does not throw exceptions.
    void WindowManager::poll_loop() noexcept {


        constexpr auto idle_interval = std::chrono::microseconds(500);


        constexpr usize backlog_threshold = 64;

        while (running_.load(std::memory_order_acquire)) {


            ZoneScopedN("WindowManager::poll_loop iteration");


            std::deque<unique_ptr<Async::Detail::TaskBase>> ready;
            {
                auto guard = pending_ops_.lock();
                ready.swap(*guard);
            }
            const bool ran_ops = !ready.empty();
            for (unique_ptr<Async::Detail::TaskBase> &task : ready) {
                task->execute();
            }


            usize drained_events = 0;
            if (!windows_.empty()) {
                for (unique_ptr<Window> &window : windows_) {
                    if (auto pumped = window->pump_events(); !pumped) {
                        Foundation::log_warn(
                            "WindowManager: background pump_events() failed for window {}: {}",
                            static_cast<usize>(window->id()),
                            pumped.error().message);
                    }
                }

                ChannelDirectory snapshot;
                {
                    auto guard = channels_.lock();
                    snapshot = *guard;
                }

                for (unique_ptr<Window> &window : windows_) {
                    WindowEventChannel *channel = find_channel(snapshot, window->id());
                    if (channel == nullptr) {
                        continue;
                    }
                    drained_events += drain_window_into(*window, *channel);


                    if (window->close_requested()) {
                        channel->close_requested.store(true, std::memory_order_release);
                    }
                    if (auto size = window->framebuffer_size()) {
                        channel->set_framebuffer_size(*size);
                    }
                }
            }


            if (ran_ops || drained_events >= backlog_threshold) {
                continue;
            }

            std::unique_lock<std::mutex> idle_lock(wake_mutex_);
            wake_cv_.wait_for(idle_lock, idle_interval, [this]() noexcept {


                if (!running_.load(std::memory_order_acquire)) {
                    return true;
                }
                auto guard = pending_ops_.lock();
                return !guard->empty();
            });
        }


        {
            std::deque<unique_ptr<Async::Detail::TaskBase>> ready;
            {
                auto guard = pending_ops_.lock();
                ready.swap(*guard);
            }
            for (unique_ptr<Async::Detail::TaskBase> &task : ready) {
                task->execute();
            }
        }


        windows_.clear();
    }

    /// Returns the current or globally available wake poll loop value.
    ///
    /// @return Returns the current wake poll loop value.
    /// @note This function does not throw exceptions.
    void WindowManager::wake_poll_loop() noexcept {
        ZoneScopedN("WindowManager::wake_poll_loop");


        {
            std::lock_guard<std::mutex> wake_lock(wake_mutex_);
        }
        wake_cv_.notify_all();
    }

    /// Pumps the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param out_events Event used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    [[nodiscard]] expected<void, WindowError> WindowManager::pump(vector<ManagedWindowEvents> &out_events) noexcept {
        ZoneScopedN("WindowManager::pump");
        if (event_thread_) {


            ChannelDirectory snapshot;
            {
                auto guard = channels_.lock();
                snapshot = *guard;
            }
            out_events.clear();
            out_events.reserve(snapshot.size());
            for (auto &[id, channel] : snapshot) {
                out_events.push_back(materialize_channel(id, *channel));
            }
            return {};
        }


        return dispatch([this, &out_events]() -> expected<void, WindowError> {
            out_events.clear();
            if (windows_.empty()) {
                return {};
            }

            for (unique_ptr<Window> &window : windows_) {
                if (auto pumped = window->pump_events(); !pumped) {
                    return unexpected(pumped.error());
                }
            }

            ChannelDirectory snapshot;
            {
                auto guard = channels_.lock();
                snapshot = *guard;
            }

            out_events.reserve(windows_.size());
            for (unique_ptr<Window> &window : windows_) {
                WindowEventChannel *channel = find_channel(snapshot, window->id());
                if (channel == nullptr) {
                    continue;
                }


                (void)drain_window_into(*window, *channel);


                if (window->close_requested()) {
                    channel->close_requested.store(true, std::memory_order_release);
                }
                if (auto size = window->framebuffer_size()) {
                    channel->set_framebuffer_size(*size);
                }

                out_events.push_back(materialize_channel(window->id(), *channel));
            }

            return {};
        });
    }

} // namespace SFT::Platform::Windowing

namespace SFT::Platform::Windowing {

    /// Spawns window.
    ///
    /// @param config Configuration values controlling the operation.
    /// @param factory `factory` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `WindowErrorCode::InvalidArgument`.
    expected<WindowId, WindowError> WindowManager::spawn_window(
        const WindowConfig &config,
        WindowFactory factory) {
        ZoneScopedN("WindowManager::spawn_window(factory)");
        if (factory == nullptr) {
            return unexpected(WindowError{
                WindowErrorCode::InvalidArgument,
                "WindowManager::spawn_window requires a non-null WindowFactory.",
            });
        }
        return dispatch([this, &config, factory]() -> expected<WindowId, WindowError> {
            auto created = factory(config);
            if (!created) {
                return unexpected(created.error());
            }
            const WindowId id = (*created)->id();
            if (!primary_window_id_) {
                primary_window_id_ = id;
            }
            windows_.push_back(std::move(*created));
            seed_channel(id);
            return id;
        });
    }

} // namespace SFT::Platform::Windowing

