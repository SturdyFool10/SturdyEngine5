#include "WindowManager.hpp"

namespace SFT::Platform::Windowing {

WindowManager::WindowManager(WindowManagerPolicy policy) noexcept
            : policy_(policy) {
            if (policy_.event_pump_mode == WindowEventPumpMode::DedicatedEventThread &&
                policy_.platform_allows_threads && compile_time_window_thread_allowed) {
                event_thread_ = std::make_unique<Async::DedicatedThread>("WindowEventThread");
            }
        }

WindowManager::~WindowManager() {
            // Tear down every window on the thread that owns them, then let event_thread_ join — mirrors
            // Application's drain-before-destroy discipline for render_thread_.
            if (event_thread_) {
                auto handle = event_thread_->run([this]() { windows_.clear(); });
                handle.wait();
            } else {
                windows_.clear();
            }
        }

[[nodiscard]] const WindowManagerPolicy &WindowManager::policy() const noexcept { return policy_; }

[[nodiscard]] bool WindowManager::has_dedicated_event_thread() const noexcept { return event_thread_ != nullptr; }

void WindowManager::destroy_window(WindowId id) noexcept {
            dispatch([this, id]() {
                std::erase_if(windows_, [id](const unique_ptr<Window> &w) { return w->id() == id; });
                if (primary_window_id_ == id) {
                    primary_window_id_ = windows_.empty() ? optional<WindowId>{} : optional<WindowId>{windows_.front()->id()};
                }
            });
        }

[[nodiscard]] optional<WindowId> WindowManager::primary_window_id() const noexcept { return primary_window_id_; }

[[nodiscard]] usize WindowManager::window_count() noexcept {
            return dispatch([this]() { return windows_.size(); });
        }

[[nodiscard]] expected<void, WindowError> WindowManager::pump(vector<ManagedWindowEvents> &out_events) noexcept {
            return dispatch([this, &out_events]() -> expected<void, WindowError> {
                out_events.clear();
                if (windows_.empty()) {
                    return {};
                }

                struct PollResult {
                    expected<ManagedWindowEvents, WindowError> events;
                };

                vector<Async::TaskHandle<PollResult>> pollers;
                pollers.reserve(windows_.size());

                for (unique_ptr<Window> &window : windows_) {
                    if (auto pumped = window->pump_events(); !pumped) {
                        return unexpected(pumped.error());
                    }
                }

                for (unique_ptr<Window> &window : windows_) {
                    Window *window_ptr = window.get();
                    const WindowId window_id = window->id();
                    pollers.push_back(Async::Scheduler::spawn([window_ptr, window_id]() -> PollResult {
                        ManagedWindowEvents collected{.window_id = window_id, .events = {}, .framebuffer_size = {}};

                        while (auto event = window_ptr->poll_event()) {
                            if (event->kind == WindowEventKind::CloseRequested) {
                                collected.close_requested = true;
                            } else if (event->kind == WindowEventKind::Resized || event->kind == WindowEventKind::FramebufferResized) {
                                collected.resized = true;
                            }
                            collected.events.push_back(*event);
                        }

                        return PollResult{std::move(collected)};
                    }));
                }

                out_events.reserve(pollers.size());
                for (Async::TaskHandle<PollResult> &poller : pollers) {
                    PollResult result = poller.wait();
                    if (!result.events) {
                        return unexpected(result.events.error());
                    }
                    out_events.push_back(std::move(*result.events));
                }

                // Sample owner-thread window state after the poller tasks have drained translated queues.
                // Backend framebuffer queries and close latches stay on the window-owner path; the worker
                // tasks only pop already-translated events from each window's protected queue.
                for (unique_ptr<Window> &window : windows_) {
                    const WindowId id = window->id();
                    ManagedWindowEvents *packet = nullptr;
                    for (ManagedWindowEvents &events : out_events) {
                        if (events.window_id == id) {
                            packet = &events;
                            break;
                        }
                    }
                    if (packet == nullptr) {
                        out_events.push_back(ManagedWindowEvents{.window_id = id, .events = {}, .framebuffer_size = {}});
                        packet = &out_events.back();
                    }
                    if (window->close_requested()) {
                        packet->close_requested = true;
                    }
                    if (auto size = window->framebuffer_size()) {
                        packet->framebuffer_size = *size;
                    }
                }

                return {};
            });
        }

} // namespace SFT::Platform::Windowing
