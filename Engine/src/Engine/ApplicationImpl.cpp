#include <Foundation/src/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <Async/src/Async.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <Engine/Application.hpp>
#include <Platform/Platform.hpp>
#include <Platform/Window/SDL3/SDL3.hpp>

using SFT::Foundation::f64;
using std::make_unique;
using std::vector;
using std::chrono::duration;
using std::chrono::high_resolution_clock;

namespace SFT::Engine {

    Application::Application(ApplicationClient &client) noexcept : client_(&client) {}

    Application::~Application() {
        drain_render_thread();
        if (engine_) {
            // drain_render_thread() only awaits each frame's CPU-side recording/submission task,
            // not the GPU's own completion of that work (the async submission model lets the CPU
            // run frames_in_flight ahead of the GPU) — client_->on_shutdown() is documented as safe
            // to destroy live GPU objects in, so the device must actually be idle first.
            engine_->wait_idle();
            client_->on_shutdown(*engine_);
        }
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
                return engine_->initialize(window, client_->application_config().engine);
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
        //
        // In practice this callback only ever fires on Windows, synchronously from inside SDL's
        // blocked interactive move/resize modal pump (see SDL3Window::sdl_repaint_watch — every other
        // platform's watch is a no-op and relies solely on the normal per-frame call in run()'s main
        // loop below). wait_for_completion=true makes each of those repaints block until fully
        // rendered before returning to the modal pump, so on-screen content tracks the drag live
        // instead of free-running ahead and catching up only once the drag ends.
        window_manager_.with_window(*id, [this, managed_ptr](Window &window) -> bool {
            window.set_repaint_callback([this, managed_ptr, window_ptr = &window]() {
                const bool resized = window_ptr->consume_resize().has_value();
                auto extent = window_ptr->framebuffer_size();
                if (!extent) {
                    return;
                }
                render_managed_window(*managed_ptr, *extent, resized, /*wait_for_completion=*/true);
            });
            return true;
        });

        return true;
    }

    void Application::render_managed_window(ManagedWindow &managed, Platform::Windowing::WindowExtent extent, bool resized, bool wait_for_completion) {
        if (resized) {
            managed.resize_pending.store(true, std::memory_order_release);
        }
        if (extent.x == 0 || extent.y == 0 || !managed.surface) {
            return;
        }

        // See wait_for_completion's doc in Application.hpp for both guards below — throttle and
        // adaptive fallback — which apply only to the Windows interactive-drag repaint path and
        // never touch the pipelined async path used everywhere else.
        bool effective_wait_for_completion = wait_for_completion;
        if (wait_for_completion) {
            constexpr f64 min_synchronous_repaint_interval_seconds = 1.0 / 120.0;
            // Threshold well above any observed healthy swapchain-recreate cost (tens of ms) but
            // far below the multi-second GPU driver stalls this guards against.
            constexpr f64 slow_repaint_fallback_threshold_seconds = 0.2;

            if (managed.last_synchronous_repaint_duration_seconds > slow_repaint_fallback_threshold_seconds) {
                effective_wait_for_completion = false;
                // Give synchronous mode another chance next repaint rather than disabling it for the
                // rest of the drag — most observed stalls were a one-off GPU wake-up, not sustained.
                managed.last_synchronous_repaint_duration_seconds = 0.0;
            } else {
                const auto throttle_now = high_resolution_clock::now();
                const f64 since_last = duration<f64>(throttle_now - managed.last_synchronous_repaint_time).count();
                if (since_last < min_synchronous_repaint_interval_seconds) {
                    return;
                }
                managed.last_synchronous_repaint_time = throttle_now;
            }
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

        // ECS extraction below is CPU-only and runs on this coordinating caller before dispatch. The
        // lambda is the one seam where graphics calls happen: on the dedicated render thread, resize
        // handling and engine_->render() both run there so all RHI/Vulkan calls for this window stay on
        // a single owning thread (Vulkan objects here are not internally synchronized).
        // resize_needed_extent is `extent`, already resolved on the main thread above — the render
        // thread must not call back into Window itself (e.g. via framebuffer_size()): on Windows the
        // main thread can be blocked inside SDL's modal move/resize pump holding Window's internal
        // lock while it waits on this very render task, so a render-thread Window touch here would
        // deadlock the whole process during a drag.
        const Core::RenderSurfaceHandle surface = *managed.surface;
        const Core::Extent2D resize_needed_extent{extent.x, extent.y};
        optional<RenderFrameParameters> frame_parameters =
            client_->request_render_frame(*engine_, surface, frame_input);
        if (!frame_parameters) {
            return;
        }
        PreparedRenderFrame prepared_frame = engine_->prepare_render_frame(surface, frame_input, *frame_parameters);
        auto render_task = [this,
                            &managed,
                            surface,
                            resize_needed_extent,
                            prepared_frame = std::move(prepared_frame)]() -> Core::RendererResult {
            if (managed.resize_pending.exchange(false, std::memory_order_acq_rel)) {
                engine_->on_surface_resize_needed(surface, resize_needed_extent);
            }
            return engine_->render(prepared_frame);
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

            if (effective_wait_for_completion) {
                const auto wait_start = high_resolution_clock::now();
                while (!managed.in_flight_frames.empty()) {
                    report_frame_result(managed.in_flight_frames.front().wait());
                    managed.in_flight_frames.pop_front();
                }
                managed.last_synchronous_repaint_duration_seconds =
                    duration<f64>(high_resolution_clock::now() - wait_start).count();
            }
        } else {
            report_frame_result(render_task());
        }
    }

    void Application::sync_window_state(const vector<Platform::Windowing::ManagedWindowEvents> &window_events) {
        using namespace Platform::Windowing;

        for (const ManagedWindowEvents &events : window_events) {
            ManagedWindow *managed = find_managed_window(events.window_id);
            if (managed == nullptr) {
                continue;
            }
            for (const Platform::Windowing::WindowEvent &event : events.events) {
                if (event.kind == WindowEventKind::FocusGained) {
                    managed->focused = true;
                } else if (event.kind == WindowEventKind::FocusLost) {
                    managed->focused = false;
                }
            }
        }

        vector<WindowSnapshot> snapshots;
        snapshots.reserve(windows_.size());
        for (auto &managed : windows_) {
            window_manager_.with_window(managed->window_id, [&](Window &window) {
                WindowSnapshot snapshot{.id = managed->window_id, .focused = managed->focused};
                if (auto size = window.size()) {
                    snapshot.size = *size;
                }
                if (auto framebuffer_size = window.framebuffer_size()) {
                    snapshot.framebuffer_size = *framebuffer_size;
                }
                if (auto position = window.position()) {
                    snapshot.position = *position;
                }
                if (auto opacity = window.opacity()) {
                    snapshot.opacity = *opacity;
                }
                snapshot.mouse_locked = window.mouse_locked();
                snapshots.push_back(snapshot);
                return true;
            });
        }

        engine_->window_state().sync(std::move(snapshots), window_manager_.primary_window_id());
    }

    bool Application::initialize() {
        using namespace Platform::Windowing;

        engine_ = make_unique<Engine>();

        // SDL3 is the default windowing backend (broad platform reach + robust Vulkan surface
        // creation). GLFW remains available - this is the one line that selects the primary window's
        // backend; spawn_managed_window<Backend> works with either.
        if (!spawn_sdl3_managed_window(client_->application_config().primary_window, /*is_primary=*/true)) {
            engine_.reset();
            Async::Scheduler::shutdown();
            return false;
        }

        if (ApplicationResult consumer_initialized = client_->on_engine_initialized(*engine_);
            !consumer_initialized) {
            Foundation::log_error("Runtime consumer initialization failed: {}", consumer_initialized.error().message);
            for (auto &managed : windows_) {
                if (managed->surface) {
                    engine_->remove_window(*managed->surface);
                    managed->surface.reset();
                }
                window_manager_.destroy_window(managed->window_id);
            }
            windows_.clear();
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
                for (const Platform::Windowing::WindowEvent &event : events.events) {
                    engine_->queue_window_event(events.window_id, event);
                }
            }
            sync_window_state(window_events);
            engine_->update();

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
                    // Removing the last surface tears down the RHI device itself (not just that
                    // window), so this is the actual point of no return for any GPU resource the
                    // client owns — earlier than ~Application() when there's no explicit "shut
                    // down now" moment otherwise. Every in-flight frame for every window has already
                    // been confirmed drained above, but that only awaited each frame's CPU-side
                    // submission task (the async submission model lets the CPU run frames_in_flight
                    // ahead of the GPU) — an explicit wait_idle() is what actually guarantees the
                    // GPU itself is done with everything client_->on_shutdown() is about to destroy.
                    if (windows_.size() == 1) {
                        engine_->wait_idle();
                        client_->on_shutdown(*engine_);
                    }
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

            const optional<f64> title_update_interval =
                client_->application_config().primary_window_title_update_interval_seconds;
            if (title_update_interval &&
                duration<f64>(now - last_title_update).count() >= *title_update_interval) {
                if (auto primary_id = window_manager_.primary_window_id()) {
                    if (ManagedWindow *primary = find_managed_window(*primary_id); primary != nullptr && !primary->closing && primary->last_delta_seconds > 0.0) {
                        const UString title = client_->primary_window_title(
                            *engine_,
                            ApplicationFrameStats{
                                .frame_seconds = primary->last_delta_seconds,
                                .frame_index = primary->frame_index,
                                .window_count = windows_.size(),
                            });
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
