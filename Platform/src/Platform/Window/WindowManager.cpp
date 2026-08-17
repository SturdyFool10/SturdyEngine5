#include <Platform/src/Platform/Window/WindowManager.hpp>
#include "WindowManager.hpp"

#include <Async/src/SpscRingBuffer.hpp>

#include <chrono>

#include <tracy/Tracy.hpp>

namespace SFT::Platform::Windowing {











    class WindowEventChannel {
      public:
        explicit WindowEventChannel(usize ring_capacity) noexcept : ring(ring_capacity) {}

        Async::SpscRingBuffer<WindowEvent> ring;



        optional<WindowEvent> pending;










        std::atomic<bool> close_requested{false};






        std::atomic<bool> resized{false};

        [[nodiscard]] bool take_resized() noexcept { return resized.exchange(false, std::memory_order_acq_rel); }

        void set_framebuffer_size(WindowExtent size) noexcept {
            const u64 packed = (static_cast<u64>(size.x) << 32U) | static_cast<u64>(size.y);
            framebuffer_size_packed_.store(packed, std::memory_order_release);
        }

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

        [[nodiscard]] WindowEventChannel *find_channel(ChannelDirectory &channels, WindowId id) noexcept {
            for (auto &[channel_id, channel] : channels) {
                if (channel_id == id) {
                    return channel.get();
                }
            }
            return nullptr;
        }





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

    WindowManager::~WindowManager() {
        ZoneScopedN("WindowManager::~WindowManager");
        if (event_thread_) {






            running_.store(false, std::memory_order_release);
            wake_poll_loop();
        } else {
            windows_.clear();
        }
    }

    [[nodiscard]] const WindowManagerPolicy &WindowManager::policy() const noexcept { return policy_; }

    [[nodiscard]] bool WindowManager::has_dedicated_event_thread() const noexcept { return event_thread_ != nullptr; }

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

    void WindowManager::seed_channel(WindowId id) noexcept {
        ZoneScopedN("WindowManager::seed_channel");
        auto channel = std::make_shared<WindowEventChannel>(policy_.max_accumulated_events_per_window);
        auto guard = channels_.lock();
        guard->emplace_back(id, std::move(channel));
    }

    [[nodiscard]] optional<WindowId> WindowManager::primary_window_id() const noexcept {
        ZoneScopedN("WindowManager::primary_window_id");
        return dispatch([this]() { return primary_window_id_; });
    }

    [[nodiscard]] usize WindowManager::window_count() noexcept {
        ZoneScopedN("WindowManager::window_count");
        return dispatch([this]() { return windows_.size(); });
    }

    void WindowManager::flush_pending(WindowEventChannel &channel) noexcept {
        if (!channel.pending.has_value()) {
            return;
        }
        push_or_warn(channel, *channel.pending);
        channel.pending.reset();
    }

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

    void WindowManager::wake_poll_loop() noexcept {
        ZoneScopedN("WindowManager::wake_poll_loop");







        {
            std::lock_guard<std::mutex> wake_lock(wake_mutex_);
        }
        wake_cv_.notify_all();
    }

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

