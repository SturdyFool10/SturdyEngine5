module;
#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <Async/Async.hpp>
#pragma endregion

module Sturdy.Engine;

import :Application;
import Sturdy.Core;
import Sturdy.Platform;
import Sturdy.Platform.SDL3;
using std::chrono::duration;
using std::chrono::high_resolution_clock;
using std::make_unique;
using std::vector;
using SFT::Foundation::f64;

namespace SFT::Engine {

    Application::Application() = default;

    Application::~Application() {
        drain_render_thread();
        render_thread_.reset();
        engine_.reset();
        Async::Scheduler::shutdown();
    }

    void Application::drain_render_thread() noexcept {
        for (auto &managed : windows_) {
            while (!managed->in_flight_frames.empty()) {
                (void)managed->in_flight_frames.front().wait();
                managed->in_flight_frames.pop_front();
            }
        }
    }

    Application::ManagedWindow *Application::find_managed_window(Platform::Windowing::WindowId id) noexcept {
        for (auto &managed : windows_) {
            if (managed->window_id == id) {
                return managed.get();
            }
        }
        return nullptr;
    }

    void Application::report_frame_result(const Core::RendererResult &result) noexcept {
        if (!result) {
            ++consecutive_render_errors_;
            if (consecutive_render_errors_ == 1 || consecutive_render_errors_ % 120 == 0) {
                Foundation::log_error("Render error: " + result.error().message);
            }
        } else {
            consecutive_render_errors_ = 0;
        }
    }

    bool Application::spawn_sdl3_managed_window(const Platform::Windowing::WindowConfig &config, bool is_primary) {
        using namespace Platform::Windowing;

        auto id = window_manager_.spawn_window<SDL3::SDL3Window>(config);
        if (!id) {
            Foundation::log_error("Failed to spawn window: {}", id.error().message);
            return false;
        }

        auto register_result = window_manager_.with_window(*id, [this, is_primary](Window &window) -> Core::RendererExpected<Core::RenderSurfaceHandle> {
            // Window-backed surface creation queries native/window-library handles and framebuffer
            // state from `window`, so keep registration on the window-owning caller path. Steady-state
            // rendering still moves onto render_thread_ below when that backend policy is selected.
            if (is_primary) {
                EngineConfig engine_config{};
                return engine_->initialize(window, engine_config);
            }
            return engine_->add_window(window);
        });

        if (!register_result) {
            Foundation::log_error("Window vanished before its render surface could be registered.");
        } else if (!*register_result) {
            Foundation::log_error("Failed to register window's render surface: {}", register_result->error().message);
        }
        if (!register_result || !*register_result) {
            window_manager_.destroy_window(*id);
            return false;
        }

        auto managed = std::make_unique<ManagedWindow>();
        managed->window_id = *id;
        managed->surface = **register_result;
        managed->primary = is_primary;
        managed->last_frame_time = std::chrono::high_resolution_clock::now();
        ManagedWindow *managed_ptr = managed.get();
        windows_.push_back(std::move(managed));

        // The repaint callback fires only from inside WindowManager::pump()'s own dispatch (a
        // Window's internals never invoke it any other way — see Window::set_repaint_callback's
        // docs), so it's always safe to touch the captured Window& directly from inside it; calling
        // back into window_manager_ from here would deadlock a single-worker DedicatedThread waiting
        // on itself.
        window_manager_.with_window(*id, [this, managed_ptr](Window &window) -> bool {
            window.set_repaint_callback([this, managed_ptr, window_ptr = &window]() {
                const bool resized = window_ptr->consume_resize().has_value();
                auto extent = window_ptr->framebuffer_size();
                if (!extent) {
                    return;
                }
                render_managed_window(*managed_ptr, *extent, resized);
            });
            return true;
        });

        return true;
    }

    void Application::render_managed_window(ManagedWindow &managed, Platform::Windowing::WindowExtent extent, bool resized) {
        if (resized) {
            managed.resize_pending.store(true, std::memory_order_release);
        }
        if (extent.x == 0 || extent.y == 0 || !managed.surface) {
            return;
        }

        constexpr f64 hitch_log_threshold_seconds = 0.1;
        const auto now = high_resolution_clock::now();
        const f64 delta_seconds = duration<f64>(now - managed.last_frame_time).count();
        managed.last_frame_time = now;
        managed.last_delta_seconds = delta_seconds;

        if (delta_seconds >= hitch_log_threshold_seconds) {
            Foundation::log_warn("Long frame detected: {}", Foundation::human_readable_time(delta_seconds));
        }

        const Core::FrameInput frame_input{
            .delta_seconds = delta_seconds,
            .frame_index = managed.frame_index,
            .framebuffer_width = extent.x,
            .framebuffer_height = extent.y,
        };
        ++managed.frame_index;

        // Everything below is the one seam where a graphics call happens. On the dedicated render
        // thread, resize handling and engine_->render() both run there so all RHI/Vulkan calls for this
        // window stay on a single owning thread (Vulkan objects here are not internally synchronized).
        const Core::RenderSurfaceHandle surface = *managed.surface;
        auto render_task = [this, &managed, surface, frame_input]() -> Core::RendererResult {
            if (managed.resize_pending.exchange(false, std::memory_order_acq_rel)) {
                engine_->on_surface_resize_needed(surface);
            }
            return engine_->render(surface, frame_input);
        };

        if (render_thread_) {
            // Bounded backpressure: never let this window's CPU submission get more than
            // max_frames_in_flight_ frames ahead of the render thread, mirroring the fence-based
            // backpressure already inside render_frame_rhi() for GPU-side frames-in-flight. Each window
            // keeps its own ring since each has its own swapchain/frame-in-flight ring on the RHI side.
            while (managed.in_flight_frames.size() >= max_frames_in_flight_) {
                report_frame_result(managed.in_flight_frames.front().wait());
                managed.in_flight_frames.pop_front();
            }
            managed.in_flight_frames.push_back(render_thread_->run(std::move(render_task)));
        } else {
            report_frame_result(render_task());
        }
    }

    bool Application::initialize() {
        using namespace Platform::Windowing;

        WindowConfig primary_config{};
        primary_config.title = "Sturdy Engine 5";
        primary_config.extent = {1280, 720};
        primary_config.graphics_api = WindowGraphicsApi::Vulkan;

        engine_ = make_unique<Engine>();

        // SDL3 is the default windowing backend (broad platform reach + robust Vulkan surface
        // creation). GLFW remains available - this is the one line that selects the primary window's
        // backend; spawn_managed_window<Backend> works with either.
        if (!spawn_sdl3_managed_window(primary_config, /*is_primary=*/true)) {
            engine_.reset();
            Async::Scheduler::shutdown();
            return false;
        }

        // Stand up the Primary Render Thread only when the backend/platform combination actually
        // recommends it (see RHI::choose_render_threading_mode) - Web and STURDY_RHI_FORCE_SINGLE_THREADED
        // builds keep render_thread_ null and fall back to the exact inline single-threaded behavior this
        // engine had before render threading existed.
        RHI::RenderThreadingCapabilities threading_caps{};
        if (Core::EngineBackend *backend = engine_->graphics_backend()) {
            threading_caps = backend->render_threading_capabilities();
        }
        if (RHI::choose_render_threading_mode(threading_caps) != RHI::RenderThreadingMode::SingleThreaded) {
            render_thread_ = make_unique<Async::DedicatedThread>("RenderThread");
        }
        max_frames_in_flight_ = std::max<u32>(1, engine_->capabilities().max_frames_in_flight);

        // Spawn a second window purely to prove simultaneous multi-window control actually works end to
        // end, not just compiles. Non-fatal if it fails - the app still runs with just the primary.
        WindowConfig demo_config{};
        demo_config.title = "Sturdy Engine 5 - Window 2";
        demo_config.extent = {960, 540};
        demo_config.graphics_api = WindowGraphicsApi::Vulkan;
        if (!spawn_sdl3_managed_window(demo_config, /*is_primary=*/false)) {
            Foundation::log_warn("Demo second window failed to start; continuing with the primary window only.");
        }

        Async::Scheduler::initialize_low_latency();
        return true;
    }

    void Application::run() {
        using namespace Platform::Windowing;

        if (windows_.empty() || !engine_) {
            return;
        }

        auto last_memory_log = high_resolution_clock::now();
        auto last_title_update = last_memory_log;
        constexpr f64 memory_log_interval_seconds = 5.0;
        constexpr f64 title_update_interval_seconds = 0.25;
        usize peak_resident_bytes = 0;

        vector<ManagedWindowEvents> window_events;

        while (!windows_.empty()) {
            // Run any work queued via Async::run_on_main_thread() before touching the window/renderer
            // this tick, since that's exactly the kind of main-thread-affined state such jobs exist to
            // touch safely.
            Async::pump_main_thread();

            if (auto pump = window_manager_.pump(window_events); !pump) {
                Foundation::log_error("Event pump failed: {}", pump.error().message);
                break;
            }

            for (const ManagedWindowEvents &events : window_events) {
                if (events.close_requested) {
                    if (ManagedWindow *managed = find_managed_window(events.window_id)) {
                        managed->closing = true;
                    }
                    continue;
                }

                ManagedWindow *managed = find_managed_window(events.window_id);
                if (managed == nullptr || managed->closing || !events.framebuffer_size) {
                    continue;
                }
                // render_managed_window() consumes any pending resize before drawing, so the rebuild path
                // is shared by both this normal-loop call and the modal-resize repaint callback.
                render_managed_window(*managed, *events.framebuffer_size, events.resized);
            }

            for (auto it = windows_.begin(); it != windows_.end();) {
                ManagedWindow &managed = **it;
                if (!managed.closing) {
                    ++it;
                    continue;
                }

                while (!managed.in_flight_frames.empty() && managed.in_flight_frames.front().is_done()) {
                    report_frame_result(managed.in_flight_frames.front().wait());
                    managed.in_flight_frames.pop_front();
                }

                if (!managed.in_flight_frames.empty()) {
                    ++it;
                    continue;
                }

                if (managed.remove_surface_task) {
                    if (!managed.remove_surface_task->is_done()) {
                        ++it;
                        continue;
                    }
                    managed.remove_surface_task->wait();
                    managed.remove_surface_task.reset();
                }

                if (managed.surface) {
                    const Core::RenderSurfaceHandle surface = *managed.surface;
                    managed.surface.reset();
                    if (render_thread_) {
                        managed.remove_surface_task = render_thread_->run([this, surface]() { engine_->remove_window(surface); });
                        ++it;
                        continue;
                    }
                    engine_->remove_window(surface);
                }

                window_manager_.destroy_window(managed.window_id);
                it = windows_.erase(it);
            }

            const auto now = high_resolution_clock::now();
            if (duration<f64>(now - last_memory_log).count() >= memory_log_interval_seconds) {
                // Keep memory telemetry off the critical path: forced mimalloc collection can walk
                // allocator state and purge pages, which is much too expensive for the render thread.
                const auto usage = Foundation::Memory::heap_usage();
                // mimalloc's peak_rss is only refreshed during allocator work and can read lower than the
                // live RSS (producing a nonsensical peak < current), so track our own high-water mark
                // from the post-collect samples instead.
                peak_resident_bytes = std::max(peak_resident_bytes, usage.current_resident_bytes);
                Foundation::log_info("Memory usage: resident={} peak_resident={} committed={} peak_committed={}",
                                     Foundation::Memory::format_bytes(usage.current_resident_bytes),
                                     Foundation::Memory::format_bytes(peak_resident_bytes),
                                     Foundation::Memory::format_bytes(usage.current_bytes),
                                     Foundation::Memory::format_bytes(usage.peak_bytes));
                last_memory_log = now;
            }

            if (duration<f64>(now - last_title_update).count() >= title_update_interval_seconds) {
                if (auto primary_id = window_manager_.primary_window_id()) {
                    if (ManagedWindow *primary = find_managed_window(*primary_id); primary != nullptr && !primary->closing && primary->last_delta_seconds > 0.0) {
                        const std::string title = std::format("SturdyEngine 5 Frame Time: {}, FPS: {:.0f} [{} window(s)]",
                                                               Foundation::human_readable_time(primary->last_delta_seconds),
                                                               1.0 / primary->last_delta_seconds, windows_.size());
                        window_manager_.with_window(*primary_id, [&](Window &w) -> bool {
                            if (auto result = w.set_title(title.c_str()); !result) {
                                Foundation::log_error("Failed to set window title: {}", result.error().message);
                            }
                            return true;
                        });
                    }
                }
                last_title_update = now;
            }
        }

        for (auto &managed : windows_) {
            managed->surface.reset();
        }
        drain_render_thread();
        engine_->wait_idle();
        engine_.reset();
        Async::Scheduler::shutdown();
    }

} // namespace SFT::Engine
