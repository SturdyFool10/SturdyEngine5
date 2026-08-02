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

[[nodiscard]] optional<WindowId> WindowManager::primary_window_id() const noexcept {
            return dispatch([this]() { return primary_window_id_; });
        }

[[nodiscard]] usize WindowManager::window_count() noexcept {
            return dispatch([this]() { return windows_.size(); });
        }

[[nodiscard]] expected<void, WindowError> WindowManager::pump(vector<ManagedWindowEvents> &out_events) noexcept {
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

                out_events.reserve(windows_.size());
                for (unique_ptr<Window> &window : windows_) {
                    ManagedWindowEvents collected{
                        .window_id = window->id(),
                        .events = {},
                        .framebuffer_size = {},
                    };

                    // Window event/state queries obey the same affinity contract as pump_events().
                    // Keep queue draining here on the dispatch owner rather than handing live Window*
                    // to Scheduler workers, even though current providers protect their queues with
                    // locks internally.
                    while (auto event = window->poll_event()) {
                        if (event->kind == WindowEventKind::CloseRequested) {
                            collected.close_requested = true;
                        } else if (event->kind == WindowEventKind::Resized ||
                                   event->kind == WindowEventKind::FramebufferResized) {
                            collected.resized = true;
                        }
                        collected.events.push_back(*event);
                    }

                    if (window->close_requested()) {
                        collected.close_requested = true;
                    }
                    if (auto size = window->framebuffer_size()) {
                        collected.framebuffer_size = *size;
                    }
                    out_events.push_back(std::move(collected));
                }

                return {};
            });
        }

} // namespace SFT::Platform::Windowing
