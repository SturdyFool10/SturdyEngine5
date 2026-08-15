#include <Foundation/src/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <Async/src/Async.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Core.hpp>
#include <Engine/Application.hpp>
#include <Platform/Platform.hpp>
#include <Platform/Window/SDL3/SDL3.hpp>

#include <tracy/Tracy.hpp>

using SFT::Foundation::f64;
using std::make_shared;
using std::make_unique;
using std::string;
using std::vector;
using std::chrono::duration;
using std::chrono::high_resolution_clock;

namespace SFT::Engine {

    Application::Application(ApplicationClient &client) noexcept : client_(&client) {}

    Application::~Application() {
        shutdown_client();
        // shutdown_client()'s drain_render_thread() already waited out every window's in-flight frames,
        // so every render thread is idle here. Explicitly join+destroy them before engine_.reset() below
        // — windows_ is declared before engine_ in Application.hpp, so relying on automatic member
        // teardown order would destroy engine_ first while a (harmlessly idle, but still live) render
        // thread object for each window still existed.
        for (auto &managed : windows_) {
            managed->render_thread.reset();
        }
        engine_.reset();
        Async::Scheduler::shutdown();
    }

    void Application::drain_render_thread() noexcept {
        for (auto &managed : windows_) {
            while (!managed->in_flight_frames.empty()) {
                report_frame_result(managed->in_flight_frames.front().wait());
                managed->in_flight_frames.pop_front();
            }
        }
    }

    void Application::shutdown_client() noexcept {
        if (!client_initialized_ || client_shutdown_ || engine_ == nullptr) {
            return;
        }
        client_shutdown_ = true;
        // CPU-side submission completion does not imply GPU completion. GameLogic shutdown may
        // release raw GPU resources, so both layers must be drained before invoking it.
        drain_render_thread();
        engine_->wait_idle();
        client_->on_shutdown(*engine_);
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
                const Core::GraphicsBackendError &error = result.error();
                Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                    .code = "engine.render.frame",
                    .summary = "frame rendering failed",
                    .context = "Engine::Application::render_managed_window",
                    .cause_code = string{Core::graphics_backend_error_code_name(error.code)},
                    .cause = error.message,
                    .details = "failure occurrence " + std::to_string(consecutive_render_errors_) +
                               "; repeated failures are reported every 120 frames",
                    .help = "inspect earlier backend diagnostics; device or surface loss may require recovery",
                });
            }
        } else {
            consecutive_render_errors_ = 0;
        }
    }

    bool Application::spawn_managed_window(
        const Platform::Windowing::WindowConfig &config,
        Platform::Windowing::WindowFactory factory,
        bool is_primary) {
        using namespace Platform::Windowing;

        expected<WindowId, WindowError> id = factory != nullptr
            ? window_manager_.spawn_window(config, factory)
            : window_manager_.spawn_window<SDL3::SDL3Window>(config);
        if (!id) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.window.spawn",
                .summary = "could not create the application window",
                .context = "Engine::Application::spawn_managed_window",
                .cause_code = string{window_error_code_name(id.error().code)},
                .cause = id.error().message,
                .details = {},
                .help = "check the selected window backend, display connection, and window configuration",
            });
            return false;
        }

        auto register_result = window_manager_.with_window(*id, [this, is_primary](Window &window) -> Core::RendererExpected<Core::RenderSurfaceHandle> {
            // Window-backed surface creation queries native/window-library handles and framebuffer
            // state from `window`, so keep registration on the window-owning caller path. Steady-state
            // rendering still moves onto this window's own render_thread below when that backend policy
            // is selected.
            if (is_primary) {
                return engine_->initialize(window, client_->application_config().engine);
            }
            return engine_->add_window(window);
        });

        if (!register_result) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.surface.window_lost",
                .summary = "the window disappeared before graphics initialization completed",
                .context = "Engine::Application::spawn_managed_window",
                .cause_code = {},
                .cause = {},
                .details = {},
                .help = "ensure the window remains alive until its render surface is registered",
            });
        } else if (!*register_result) {
            const Core::GraphicsBackendError &error = register_result->error();
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.surface.register",
                .summary = "could not create a render surface for the application window",
                .context = "Engine::Application::spawn_managed_window",
                .cause_code = string{Core::graphics_backend_error_code_name(error.code)},
                .cause = error.message,
                .details = {},
                .help = "verify graphics feature requirements and presentation support for this display",
            });
        }
        if (!register_result || !*register_result) {
            window_manager_.destroy_window(*id);
            return false;
        }

        auto managed = std::make_unique<ManagedWindow>();
        managed->window_id = *id;
        managed->surface = **register_result;
        managed->primary = is_primary;
        managed->window_snapshot = WindowSnapshot{
            .id = *id,
            .size = config.extent,
            .framebuffer_size = config.extent,
            .position = config.position,
        };
        managed->live_resize = make_shared<LiveResizeState>();
        managed->last_frame_time = std::chrono::high_resolution_clock::now();
        // use_render_threading_ is false here for the very first (primary) call — the backend doesn't
        // exist yet to query render_threading_capabilities() from, see initialize()'s own comment,
        // which sets up the primary window's render_thread itself right after this returns. Every
        // later call (secondary/tear-off windows) picks it up immediately.
        if (use_render_threading_) {
            managed->render_thread = make_unique<Async::DedicatedThread>("RenderThread-" + std::to_string(static_cast<usize>(*id)));
        }
        ManagedWindow *managed_ptr = managed.get();
        windows_.push_back(std::move(managed));

        // Windows can invoke the provider callback from its event-producing thread while its modal
        // move/resize loop is active. It must not render or query Window there: publishing one packed
        // extent is lock-free, lifetime-safe, and lets run() remain this application's only render
        // coordinator.
        const shared_ptr<LiveResizeState> live_resize = managed_ptr->live_resize;
        window_manager_.with_window(*id, [live_resize](Window &window) -> bool {
            window.set_live_resize_callback([live_resize](WindowExtent extent) {
                live_resize->publish(extent);
            });
            return true;
        });

        return true;
    }

    optional<Core::RenderSurfaceHandle> Application::spawn_secondary_window(
        const Platform::Windowing::WindowConfig &config,
        Platform::Windowing::WindowFactory factory) {
        if (!engine_ || !client_->application_config().enable_runtime_window_management) {
            return std::nullopt;
        }
        const Platform::Windowing::WindowFactory effective_factory =
            factory != nullptr ? factory : client_->application_config().primary_window_factory;
        if (!spawn_managed_window(config, effective_factory, /*is_primary=*/false)) {
            return std::nullopt;
        }
        // spawn_managed_window() always appends the newly registered window as the last entry of
        // windows_ (see its own body) — windows_.back() is exactly the one just spawned, since the
        // only other mutator of windows_ (run()'s own loop) is single-threaded and this call is
        // always made from that same thread.
        return windows_.back()->surface;
    }

    void Application::request_close_window(Platform::Windowing::WindowId id) noexcept {
        if (ManagedWindow *managed = find_managed_window(id)) {
            managed->closing = true;
        }
    }

    void Application::process_window_requests() {
        if (!engine_) {
            return;
        }
        for (WindowRequest &request : engine_->window_requests().drain()) {
            if (auto *spawn = std::get_if<SpawnWindowRequest>(&request)) {
                const Platform::Windowing::WindowConfig config = spawn->window.view();
                const optional<Core::RenderSurfaceHandle> surface = spawn_secondary_window(config, spawn->factory);
                engine_->window_requests().complete(WindowRequestCompletion{
                    .id = spawn->id,
                    .kind = WindowRequestKind::Spawn,
                    .accepted = surface.has_value(),
                    .surface = surface,
                    .window = surface ? surface->window_id : Platform::Windowing::WindowId{},
                    .message = surface ? UString{} : UString{"Runtime window management is disabled or window creation failed."},
                });
                continue;
            }

            // Every state request below is posted rather than dispatched-and-waited-for. None of them
            // has a result this loop consumes, and blocking on the window-owning thread here is exactly
            // what used to freeze rendering for the whole of an interactive resize: that thread sits
            // inside Windows' modal move/resize loop until the user releases the mouse, so one cursor
            // request issued mid-drag stalled the entire application loop behind it. See
            // WindowManager::post_to_window().
            if (auto *cursor = std::get_if<SetCursorIconRequest>(&request)) {
                // A UI re-requests its hovered cursor every frame, so this is by far the highest-volume
                // request kind. Collapse the unchanged repeats here rather than posting thousands of
                // identical ops that the window thread would have to drain (in a burst, once a modal
                // resize ends) to no visible effect.
                ManagedWindow *managed = find_managed_window(cursor->window);
                if (managed != nullptr && managed->applied_cursor_icon == cursor->icon) {
                    continue;
                }
                if (managed != nullptr) {
                    managed->applied_cursor_icon = cursor->icon;
                }
                window_manager_.post_to_window(cursor->window, [icon = cursor->icon](Platform::Windowing::Window &w) {
                    return w.set_cursor_icon(icon);
                });
                continue;
            }

            if (auto *fullscreen = std::get_if<SetFullscreenRequest>(&request)) {
                window_manager_.post_to_window(fullscreen->window, [mode = fullscreen->mode](Platform::Windowing::Window &w) {
                    return w.set_fullscreen(mode);
                });
                continue;
            }

            if (auto *decorated = std::get_if<SetDecoratedRequest>(&request)) {
                window_manager_.post_to_window(decorated->window, [enabled = decorated->decorated](Platform::Windowing::Window &w) {
                    return w.set_decorated(enabled);
                });
                continue;
            }

            if (auto *transparent = std::get_if<SetTransparentRequest>(&request)) {
                window_manager_.post_to_window(transparent->window, [enabled = transparent->transparent](Platform::Windowing::Window &w) {
                    return w.set_transparent(enabled);
                });
                continue;
            }

            if (auto *blur = std::get_if<SetBlurRequest>(&request)) {
                window_manager_.post_to_window(blur->window, [kind = blur->kind, enabled = blur->enabled](Platform::Windowing::Window &w) {
                    return w.set_effect(Platform::Windowing::WindowEffect{kind, enabled});
                });
                continue;
            }

            if (auto *area = std::get_if<SetTextInputAreaRequest>(&request)) {
                window_manager_.post_to_window(area->window, [value = area->area](Platform::Windowing::Window &w) {
                    return w.set_text_input_area(value);
                });
                continue;
            }

            if (auto *active = std::get_if<SetTextInputActiveRequest>(&request)) {
                window_manager_.post_to_window(active->window, [enabled = active->active](Platform::Windowing::Window &w) {
                    return enabled ? w.start_text_input() : w.stop_text_input();
                });
                continue;
            }

            const CloseWindowRequest &close = std::get<CloseWindowRequest>(request);
            ManagedWindow *target = find_managed_window(close.window);
            if (target != nullptr && !target->pending_close_completion) {
                request_close_window(close.window);
                // Completion is posted once this window is actually fully torn down (run()'s closing
                // loop), not here — see pending_close_completion's own doc comment (Application.hpp)
                // for why firing it this early raced a still-in-flight render thread.
                target->pending_close_completion = close.id;
            } else if (target != nullptr) {
                // Already closing from an earlier request (e.g. DockWindowCoordinator's own
                // close_requests_ bookkeeping should prevent this, but stay correct regardless of the
                // caller) — the window's fate is already sealed, so satisfy this request immediately
                // rather than orphaning its id (only one pending_close_completion slot exists per window).
                engine_->window_requests().complete(WindowRequestCompletion{
                    .id = close.id,
                    .kind = WindowRequestKind::Close,
                    .accepted = true,
                    .window = close.window,
                    .message = {},
                });
            } else {
                engine_->window_requests().complete(WindowRequestCompletion{
                    .id = close.id,
                    .kind = WindowRequestKind::Close,
                    .accepted = false,
                    .window = close.window,
                    .message = "Managed window was not found.",
                });
            }
        }
    }

    bool Application::render_managed_window(ManagedWindow &managed,
                                             Platform::Windowing::WindowExtent extent,
                                             bool resized,
                                             bool coalesce_if_backpressured) {
        if (extent.x == 0 || extent.y == 0 || !managed.surface) {
            return false;
        }

        if (managed.render_thread && coalesce_if_backpressured) {
            // A live resize can generate many pixel-size events while a driver is still recreating
            // the previous swapchain. Only one submitted frame can be useful: retain the newest
            // unscheduled extent instead of queuing another obsolete swapchain recreation.
            while (!managed.in_flight_frames.empty() && managed.in_flight_frames.front().is_done()) {
                report_frame_result(managed.in_flight_frames.front().wait());
                managed.in_flight_frames.pop_front();
            }
            if (!managed.in_flight_frames.empty()) {
                return false;
            }
        }

        const u64 packed_extent =
            (static_cast<u64>(extent.x) << 32U) | static_cast<u64>(extent.y);
        if (resized) {
            // Do not use a bare dirty bit here. The render thread can still have an older live-resize
            // task queued when SDL delivers the final committed extent; that older task must not steal
            // the final notification and recreate at its stale captured size.
            managed.pending_resize_extent.store(packed_extent, std::memory_order_release);
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
            .live_resize = coalesce_if_backpressured,
        };
        ++managed.frame_index;

        // ECS extraction below is CPU-only and runs on this coordinating caller before dispatch. The
        // lambda is the one seam where graphics calls happen: on the dedicated render thread, resize
        // handling and engine_->render() both run there so all RHI/Vulkan calls for this window stay on
        // a single owning thread (Vulkan objects here are not internally synchronized).
        const Core::RenderSurfaceHandle surface = *managed.surface;
        optional<RenderFrameParameters> frame_parameters =
            client_->request_render_frame(*engine_, surface, frame_input);
        if (!frame_parameters) {
            return true;
        }
        PreparedRenderFrame prepared_frame = engine_->prepare_render_frame(surface, frame_input, *frame_parameters);
        auto render_task = [this,
                            &managed,
                            surface,
                            extent,
                            packed_extent,
                            resized,
                            prepared_frame = std::move(prepared_frame)]() -> Core::RendererResult {
            if (resized) {
                // Only this task may consume a notification for this exact extent. If a newer resize
                // was accepted while this task waited in the render queue, leave that newer request
                // intact for its corresponding task instead of resizing the surface backwards.
                u64 expected_extent = packed_extent;
                if (managed.pending_resize_extent.compare_exchange_strong(
                        expected_extent, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    engine_->on_surface_resize_needed(surface, extent);
                }
            }
            return engine_->render(prepared_frame);
        };

        if (managed.render_thread) {
            // Bounded backpressure: never let this window's CPU submission get more than
            // max_frames_in_flight_ frames ahead of the render thread, mirroring the fence-based
            // backpressure already inside render_frame_rhi() for GPU-side frames-in-flight. Each window
            // keeps its own ring since each has its own swapchain/frame-in-flight ring on the RHI side.
            while (managed.in_flight_frames.size() >= max_frames_in_flight_) {
                report_frame_result(managed.in_flight_frames.front().wait());
                managed.in_flight_frames.pop_front();
            }
            managed.in_flight_frames.push_back(managed.render_thread->run(std::move(render_task)));
        } else {
            report_frame_result(render_task());
        }
        return true;
    }

    void Application::sync_window_state(const vector<Platform::Windowing::ManagedWindowEvents> &window_events) {
        using namespace Platform::Windowing;

        for (const ManagedWindowEvents &events : window_events) {
            ManagedWindow *managed = find_managed_window(events.window_id);
            if (managed == nullptr) {
                continue;
            }

            WindowSnapshot &snapshot = managed->window_snapshot;
            // While Windows owns its modal resize loop, the channel still carries its last committed
            // framebuffer size. The live-resize callback has a newer extent in window_snapshot, so do
            // not overwrite it with that stale sample before UI/game systems consume WindowState.
            if (events.framebuffer_size && !managed->live_resize_active) {
                snapshot.framebuffer_size = *events.framebuffer_size;
            }
            for (const Platform::Windowing::WindowEvent &event : events.events) {
                switch (event.kind) {
                    case WindowEventKind::FocusGained:
                        managed->focused = true;
                        break;
                    case WindowEventKind::FocusLost:
                        managed->focused = false;
                        break;
                    case WindowEventKind::Moved:
                        snapshot.position = event.position;
                        break;
                    case WindowEventKind::Resized:
                        snapshot.size = event.resize.current;
                        if (event.resize.framebuffer_changed) {
                            snapshot.framebuffer_size = event.resize.framebuffer;
                        }
                        break;
                    case WindowEventKind::FramebufferResized:
                        snapshot.framebuffer_size = event.resize.framebuffer;
                        break;
                    case WindowEventKind::MouseLocked:
                        snapshot.mouse_locked = true;
                        break;
                    case WindowEventKind::MouseUnlocked:
                        snapshot.mouse_locked = false;
                        break;
                    default:
                        break;
                }
            }
            snapshot.focused = managed->focused;
        }

        vector<WindowSnapshot> snapshots;
        snapshots.reserve(windows_.size());
        optional<WindowId> primary_window;
        for (const auto &managed : windows_) {
            snapshots.push_back(managed->window_snapshot);
            if (managed->primary) {
                primary_window = managed->window_id;
            }
        }

        engine_->window_state().sync(std::move(snapshots), primary_window);
    }

    bool Application::initialize() {
        using namespace Platform::Windowing;

        const Foundation::Stopwatch stopwatch;
        client_initialized_ = false;
        client_shutdown_ = false;
        engine_ = make_unique<Engine>();

        // SDL3 is the built-in default. Optional providers are selected by an explicit factory
        // reference in ApplicationConfig, preserving static dead-stripping when they are unselected.
        const ApplicationConfig &config = client_->application_config();
        if (!spawn_managed_window(
                config.primary_window,
                config.primary_window_factory,
                /*is_primary=*/true)) {
            engine_.reset();
            Async::Scheduler::shutdown();
            return false;
        }

        if (ApplicationResult consumer_initialized = client_->on_engine_initialized(*engine_);
            !consumer_initialized) {
            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                .code = "engine.game_logic.initialize",
                .summary = "game or application logic failed to initialize",
                .context = "Engine::Application::initialize",
                .cause_code = "engine.game_logic.failure",
                .cause = consumer_initialized.error().message.cpp_string(),
                .details = {},
                .help = "resolve the reported content, resource, or system setup failure and restart",
            });
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
        client_initialized_ = true;

        // Decide once whether windows get a real render thread each, only when the backend/platform
        // combination actually recommends it (see RHI::choose_render_threading_mode) - Web and
        // STURDY_RHI_FORCE_SINGLE_THREADED builds keep this false and every window falls back to the
        // exact inline single-threaded behavior this engine had before render threading existed. This
        // can only be decided after engine_->graphics_backend() exists, i.e. after the primary window's
        // spawn_managed_window() call above already ran (chicken-and-egg: the backend doesn't exist
        // until the primary surface is created) — every *later* spawn_managed_window() call reads
        // use_render_threading_ directly, but the primary window's own render_thread has to be set up
        // here explicitly since it missed that assignment.
        RHI::RenderThreadingCapabilities threading_caps{};
        if (Core::EngineBackend *backend = engine_->graphics_backend()) {
            threading_caps = backend->render_threading_capabilities();
        }
        use_render_threading_ = RHI::choose_render_threading_mode(threading_caps) != RHI::RenderThreadingMode::SingleThreaded;
        Foundation::log_info(
            "Application: per-window render threading {} (dedicated_thread={} parallel_recording={} platform_allows={}).",
            use_render_threading_ ? "enabled" : "disabled",
            threading_caps.backend_allows_dedicated_render_thread,
            threading_caps.backend_allows_parallel_command_recording,
            threading_caps.platform_allows_threads);
        if (use_render_threading_ && !windows_.empty()) {
            windows_.front()->render_thread =
                make_unique<Async::DedicatedThread>("RenderThread-" + std::to_string(static_cast<usize>(windows_.front()->window_id)));
        }
        // engine_->capabilities().max_frames_in_flight is already >= 1 by construction — resolved
        // exactly once, through Core::resolve_frames_in_flight, at backend initialization
        // (Core/Vulkan/VulkanBackendDevice.cpp) — no local zero-guard needed here.
        max_frames_in_flight_ = engine_->capabilities().max_frames_in_flight;

        Async::Scheduler::initialize_low_latency();
        Foundation::log_info("Application initialized in {}", stopwatch.elapsed_human());
        return true;
    }

    void Application::run() {
        using namespace Platform::Windowing;

        if (windows_.empty() || !engine_) {
            return;
        }

        auto last_memory_log = high_resolution_clock::now();
        auto last_title_update = last_memory_log;
        auto last_tick_time = last_memory_log;
        constexpr f64 memory_log_interval_seconds = 5.0;
        usize peak_resident_bytes = 0;

        vector<ManagedWindowEvents> window_events;

        while (!windows_.empty()) {
            // Run any work queued via Async::run_on_main_thread() before touching the window/renderer
            // this tick, since that's exactly the kind of main-thread-affined state such jobs exist to
            // touch safely.
            Async::pump_main_thread();

            if (auto pump = window_manager_.pump(window_events); !pump) {
                Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                    .code = "engine.events.pump",
                    .summary = "the platform event loop failed",
                    .context = "Engine::Application::run",
                    .cause_code = string{window_error_code_name(pump.error().code)},
                    .cause = pump.error().message,
                    .details = {},
                    .help = "the application will stop cleanly; inspect the window backend state",
                });
                break;
            }

            for (const ManagedWindowEvents &events : window_events) {
                for (const Platform::Windowing::WindowEvent &event : events.events) {
                    engine_->queue_window_event(events.window_id, event);
                }
            }

            // SDL's Windows event watch publishes the current pixel extent even while its modal
            // move/resize loop prevents the normal event pump from returning. Consume the newest one
            // here, on the sole render-coordinating thread. A bounded queue keeps a slow driver from
            // turning every intermediate size into an obsolete swapchain recreation.
            //
            // The retry cadence is intentionally display-like rather than event-like. Window systems
            // can produce hundreds of size updates per second, while each Vulkan swapchain rebuild is
            // a WSI transaction. At most one attempt per ~16 ms retains fluid feedback, lets the
            // compositor scale the most recent image between attempts, and avoids serializing a drag
            // behind redundant recreate requests.
            constexpr auto live_resize_submission_interval = std::chrono::milliseconds(16);
            constexpr auto live_resize_settle_interval = std::chrono::milliseconds(250);
            // Backoff for an attempt the renderer declined because this window's previous present is
            // still in the WSI. Short enough that the retry lands essentially as soon as the present
            // completes, but long enough that the application loop isn't re-attempting (and re-logging)
            // the same coalesced frame thousands of times a second while it waits.
            constexpr auto live_resize_retry_interval = std::chrono::milliseconds(1);
            const auto live_resize_now = high_resolution_clock::now();
            for (auto &managed_ptr : windows_) {
                ManagedWindow &managed = *managed_ptr;
                if (managed.live_resize) {
                    if (optional<WindowExtent> extent = managed.live_resize->consume()) {
                        managed.pending_live_resize = *extent;
                        // Publish the same fresh physical extent to WindowState before update() and
                        // request_render_frame(). This makes immediate-mode UI layout observe the
                        // current tab bounds in the same tick as the render path, rather than slowly
                        // catching up from SDL's last pre-modal framebuffer sample.
                        managed.window_snapshot.framebuffer_size = *extent;
                        // The matching normal event is unavailable until SDL returns from the Windows
                        // modal pump. Keep this state latched until that event arrives rather than
                        // guessing based on elapsed time: users can pause mid-drag indefinitely.
                        managed.live_resize_active = true;
                    }
                }

                if (managed.pending_live_resize) {
                    // Interactive resize is a presentation-only redraw. Reset the presentation-frame
                    // timestamp so time spent in Windows' modal loop is neither fed into temporal
                    // rendering nor reported as an engine frame hitch. Retain the newest extent after
                    // submission: a render task can intentionally skip while its previous present is
                    // still in the WSI, and the next cadence tick must retry it without waiting for a
                    // new mouse-move message.
                    managed.last_frame_time = live_resize_now;
                    const bool extent_changed = !managed.submitted_live_resize ||
                        managed.submitted_live_resize->x != managed.pending_live_resize->x ||
                        managed.submitted_live_resize->y != managed.pending_live_resize->y;
                    // A size this window has not tried to render yet goes out immediately, so the
                    // first frame at each new extent is never held back by the cadence timer. Tracked
                    // separately from submitted_live_resize (which only advances on an accepted frame)
                    // precisely so a *declined* attempt doesn't re-qualify as "new" on the very next
                    // iteration and spin the loop.
                    const bool unattempted = !managed.attempted_live_resize ||
                        managed.attempted_live_resize->x != managed.pending_live_resize->x ||
                        managed.attempted_live_resize->y != managed.pending_live_resize->y;
                    if (unattempted || live_resize_now >= managed.next_live_resize_submission) {
                        managed.attempted_live_resize = managed.pending_live_resize;
                        const bool submitted = render_managed_window(
                            managed, *managed.pending_live_resize, extent_changed,
                            /*coalesce_if_backpressured=*/true);
                        if (submitted) {
                            managed.submitted_live_resize = managed.pending_live_resize;
                            managed.next_live_resize_submission =
                                live_resize_now + live_resize_submission_interval;
                        } else {
                            managed.next_live_resize_submission =
                                live_resize_now + live_resize_retry_interval;
                        }
                    }
                }

                if (managed.pending_live_resize || managed.live_resize_active) {
                    managed.last_frame_time = live_resize_now;
                }
            }

            // Once SDL has returned from its modal pump, its regular resize event is authoritative
            // again. Let the ordinary path issue one final frame and resume live Window queries.
            for (const ManagedWindowEvents &events : window_events) {
                if (events.resized) {
                    if (ManagedWindow *managed = find_managed_window(events.window_id)) {
                        managed->pending_live_resize.reset();
                        // Keep submitted_live_resize through the normal-render loop below. When the
                        // final SDL framebuffer extent matches it, that loop can retain the already
                        // recreated composition presenter instead of needlessly rebuilding it at the
                        // same size (a one-frame flash on mouse release).
                        managed->attempted_live_resize.reset();
                        managed->live_resize_active = false;
                        managed->live_resize_nonblocking_until =
                            live_resize_now + live_resize_settle_interval;
                    }
                }
            }

            // WindowState comes exclusively from the event-channel cache, so it remains safe to
            // publish while the dedicated event thread is inside Windows' modal resize loop.
            sync_window_state(window_events);

            const auto tick_now = high_resolution_clock::now();
            const f64 tick_delta_seconds = duration<f64>(tick_now - last_tick_time).count();
            last_tick_time = tick_now;
            engine_->update(tick_delta_seconds);
            process_window_requests();

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
                if (managed->live_resize_active) {
                    // The channel still reports its last pre-modal framebuffer extent. Rendering it
                    // would immediately undo the fresh extent consumed above and recreate again.
                    continue;
                }
                const bool in_live_resize_settle_interval =
                    high_resolution_clock::now() < managed->live_resize_nonblocking_until;
                const bool final_extent_already_submitted =
                    managed->submitted_live_resize &&
                    managed->submitted_live_resize->x == events.framebuffer_size->x &&
                    managed->submitted_live_resize->y == events.framebuffer_size->y;
                // The live path has already scheduled this exact extent. Re-marking it dirty here
                // would turn the final SDL notification into a same-size composition-presenter
                // rebuild, briefly detaching its visual and flashing on mouse release. A genuinely
                // different final extent still takes the normal authoritative resize path.
                const bool resize_requires_notification = events.resized && !final_extent_already_submitted;
                (void)render_managed_window(
                    *managed, *events.framebuffer_size, resize_requires_notification,
                    in_live_resize_settle_interval);
                if (events.resized) {
                    managed->submitted_live_resize.reset();
                }
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
                        shutdown_client();
                    }
                    const Core::RenderSurfaceHandle surface = *managed.surface;
                    managed.surface.reset();
                    if (managed.render_thread) {
                        managed.remove_surface_task = managed.render_thread->run([this, surface]() { engine_->remove_window(surface); });
                        ++it;
                        continue;
                    }
                    engine_->remove_window(surface);
                }

                const bool was_primary = managed.primary;
                window_manager_.destroy_window(managed.window_id);
                // Posted only now — surface removal (including this window's render thread's own
                // remove_surface_task, confirmed done above) is fully complete, so a completion
                // handler is safe to destroy any GPU resources tied to this window. See
                // pending_close_completion's own doc comment (Application.hpp).
                if (managed.pending_close_completion) {
                    engine_->window_requests().complete(WindowRequestCompletion{
                        .id = *managed.pending_close_completion,
                        .kind = WindowRequestKind::Close,
                        .accepted = true,
                        .window = managed.window_id,
                        .message = {},
                    });
                }
                it = windows_.erase(it);
                if (was_primary && !windows_.empty()) {
                    windows_.front()->primary = true;
                }
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
                ManagedWindow *primary = nullptr;
                for (const auto &managed : windows_) {
                    if (managed->primary) {
                        primary = managed.get();
                        break;
                    }
                }
                if (primary != nullptr && !primary->closing && !primary->live_resize_active &&
                    primary->last_delta_seconds > 0.0) {
                    const UString title = client_->primary_window_title(
                        *engine_,
                        ApplicationFrameStats{
                            .frame_seconds = primary->last_delta_seconds,
                            .frame_index = primary->frame_index,
                            .window_count = windows_.size(),
                        });
                    // Posted, never waited on — the periodic title update is cosmetic, and blocking on
                    // the window thread for it froze the loop for a whole interactive resize whenever
                    // the interval happened to elapse mid-drag (that thread is inside an OS modal loop
                    // until the drag ends). See WindowManager::post_to_window().
                    window_manager_.post_to_window(primary->window_id, [title](Window &w) -> bool {
                        if (auto result = w.set_title(title.c_str()); !result) {
                            Foundation::log_diagnostic(Foundation::ConsoleDiagnostic{
                                .severity = Foundation::DiagnosticSeverity::Warning,
                                .code = "engine.window.title",
                                .summary = "could not update the primary window title",
                                .context = "Engine::Application::run",
                                .cause_code = string{window_error_code_name(result.error().code)},
                                .cause = result.error().message,
                                .details = {},
                                .help = "rendering will continue; disable periodic title updates if unsupported",
                            });
                        }
                        return true;
                    });
                }
                last_title_update = now;
            }

            FrameMark;
        }

        for (auto &managed : windows_) {
            managed->surface.reset();
        }
        shutdown_client();
        engine_.reset();
        Async::Scheduler::shutdown();
    }

} // namespace SFT::Engine
