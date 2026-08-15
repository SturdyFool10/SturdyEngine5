#pragma once

#include <Foundation/src/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <Async/src/Async.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <vector>
#pragma endregion

#include "EngineModule.hpp"
#include "GameLogic.hpp"
#include "WindowState.hpp"
#include <Core/Core.hpp>
#include <Platform/Platform.hpp>

using std::atomic;
using std::optional;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

namespace SFT::Engine {

    struct ApplicationConfig {
        Platform::Windowing::WindowConfig primary_window;
        // Null selects Application's built-in SDL3 path. Optional providers supply an explicit factory
        // symbol here, so merely building or linking their static archives has no binary footprint.
        Platform::Windowing::WindowFactory primary_window_factory = nullptr;
        EngineConfig engine;
        // No periodic title mutation unless the consumer explicitly enables it.
        optional<f64> primary_window_title_update_interval_seconds;
        // Gates spawn_secondary_window()/WindowRequests-driven spawning (docking tear-off, editor
        // windows) — window/surface *creation* always runs on the caller/main thread regardless (see
        // spawn_managed_window()), never on a render thread, so this has no effect on render
        // threading (each ManagedWindow gets its own render thread independent of this flag — see
        // Application::use_render_threading_'s doc comment).
        bool enable_runtime_window_management = false;
    };

    struct ApplicationFrameStats {
        f64 frame_seconds = 0.0;
        u64 frame_index = 0;
        usize window_count = 0;
    };

    // Compatibility names retained while Application migrates from the combined host/client API.
    using ApplicationError = GameLogicError;
    using ApplicationResult = GameLogicResult;

    // Desktop-host policy layered over host-independent GameLogic. Runtime supplies this policy for a
    // delivered app; a future Editor drives GameLogic directly with its own windows, titles, viewport,
    // pause/step, and process lifetime instead of inheriting Runtime's choices.
    class ApplicationClient : public GameLogic {
      public:
        ~ApplicationClient() override = default;

        [[nodiscard]] virtual const ApplicationConfig &application_config() const noexcept = 0;
        [[nodiscard]] virtual UString primary_window_title(
            Engine &engine,
            const ApplicationFrameStats &stats) = 0;
    };

    // Process host: owns the WindowManager and the engine, runs the main loop, and forwards OS events,
    // resizes and frame timing into the engine for every managed window. This is the boundary where the
    // platform/OS lives; everything below Engine is platform- and API-agnostic.
    class Application {
      public:
        explicit Application(ApplicationClient &client) noexcept;
        ~Application();

        bool initialize();
        void run();

        // Spawns an additional OS window at runtime (after initialize() has already run), backed by
        // its own render surface via Engine::add_window() — the same non-primary path
        // spawn_managed_window() below already takes for every window after the first, just exposed
        // here for the first time. A null `factory` inherits ApplicationConfig::primary_window_factory
        // (and therefore the built-in SDL3 path when the primary also uses it). Available only when
        // enable_runtime_window_management is true; returns nullopt otherwise or if creation fails.
        // On success, the
        // returned handle is what a caller passes to request_close_window() (via its window_id) and
        // to Engine/GameLogic APIs that take a Core::RenderSurfaceHandle for this window.
        [[nodiscard]] optional<Core::RenderSurfaceHandle> spawn_secondary_window(
            const Platform::Windowing::WindowConfig &config,
            Platform::Windowing::WindowFactory factory = nullptr);

        // Marks a managed window for teardown on run()'s own next tick, reusing the exact
        // close/drain/remove_window/destroy_window sequence run() already applies when the OS itself
        // reports a close request (events.close_requested) — just triggered programmatically instead
        // of from an OS event, e.g. a docking workspace's last panel in a torn-off window closing.
        // A no-op if `id` isn't currently a managed window (already closing, or never was one).
        void request_close_window(Platform::Windowing::WindowId id) noexcept;

      private:
        // Thread-safe handoff owned both by ManagedWindow and the SDL event-watch callback. The callback
        // publishes only the latest physical framebuffer extent; Application::run() is the sole render
        // coordinator and consumes it on the application thread.
        struct LiveResizeState {
            void publish(Platform::Windowing::WindowExtent extent) noexcept {
                const u64 packed = (static_cast<u64>(extent.x) << 32U) | static_cast<u64>(extent.y);
                pending_extent.store(packed, std::memory_order_release);
            }

            [[nodiscard]] optional<Platform::Windowing::WindowExtent> consume() noexcept {
                const u64 packed = pending_extent.exchange(0, std::memory_order_acq_rel);
                if (packed == 0) {
                    return std::nullopt;
                }
                return Platform::Windowing::WindowExtent{
                    static_cast<u32>(packed >> 32U),
                    static_cast<u32>(packed & 0xFFFFFFFFU),
                };
            }

          private:
            atomic<u64> pending_extent{0};
        };

        // Per-window render bookkeeping — everything render_managed_window() needs that isn't shared
        // app-wide state. Held by pointer in windows_ so the vector can grow (spawning more windows at
        // runtime) without invalidating references captured by in-flight render tasks.
        struct ManagedWindow {
            Platform::Windowing::WindowId window_id{};
            optional<Core::RenderSurfaceHandle> surface;
            bool primary = false;
            bool closing = false;
            // Latched from WindowEventKind::FocusGained/FocusLost — Window exposes no direct getter
            // for this, only the one-shot event (see WindowState.hpp).
            bool focused = false;
            WindowSnapshot window_snapshot{};
            // Last cursor icon actually posted to this window. A UI re-requests its hovered icon every
            // frame, so process_window_requests() compares against this and drops the unchanged repeats
            // instead of posting an op per frame. Empty until the first request for this window.
            optional<Platform::Windowing::CursorIcon> applied_cursor_icon;
            // The requested extent is carried alongside the resize notification. A boolean lets an
            // older queued live-resize task consume a newer final SDL notification and recreate the
            // surface at its stale extent; the task for an extent may only consume its own request.
            // All renderable framebuffer extents are non-zero, so zero is the empty sentinel.
            atomic<u64> pending_resize_extent{0};
            shared_ptr<LiveResizeState> live_resize;
            optional<Platform::Windowing::WindowExtent> pending_live_resize;
            // Last extent handed to the renderer from pending_live_resize. Keeping it separate lets
            // Application retry a frame while the WSI catches up without needlessly marking the
            // already-current swapchain dirty again. It also survives until the final ordinary SDL
            // resize event is consumed, so that identical handoff extent does not force a same-size
            // DirectComposition presenter rebuild.
            optional<Platform::Windowing::WindowExtent> submitted_live_resize;
            // Last extent this window *tried* to render, whether or not the renderer accepted it.
            // Distinct from submitted_live_resize so a declined attempt (previous present still in
            // flight) doesn't immediately look like an untried new size and re-attempt on every single
            // application iteration — see run()'s live-resize section.
            optional<Platform::Windowing::WindowExtent> attempted_live_resize;
            std::chrono::high_resolution_clock::time_point next_live_resize_submission{};
            // Keeps the final authoritative resize event on the same nonblocking present path for a
            // short settling interval after a modal loop ends. This avoids one last synchronous wait
            // undoing the responsiveness maintained throughout the drag.
            std::chrono::high_resolution_clock::time_point live_resize_nonblocking_until{};
            bool live_resize_active = false;
            std::deque<Async::TaskHandle<Core::RendererResult>> in_flight_frames;
            optional<Async::TaskHandle<void>> remove_surface_task;
            // Set only when this window is closing because of a WindowRequests::close() call (not an
            // OS-level close, e.g. the titlebar X or Alt+F4 — see events.close_requested's own path,
            // which never touches this). The completion is posted once this window is actually fully
            // torn down (in-flight frames drained, surface removed — see run()'s closing loop), not
            // when the request is merely accepted: a GameLogic's WindowRequestKind::Close handler (e.g.
            // WorkbenchUi::process_window_completions destroying that window's UI renderer resources)
            // needs the guarantee that this window's render thread is truly done with everything by the
            // time it observes the completion, or it can destroy GPU resources the render thread is
            // still mid-frame with — see memory project_multi_window_render_threading for the
            // free(): invalid pointer this caused when completion used to fire immediately on accept.
            optional<WindowRequestId> pending_close_completion;
            // Each window gets its own dedicated render thread (rather than every window sharing one
            // Application-wide thread) so recording/submitting/presenting for separate OS windows —
            // most commonly several torn-off docking panels — actually runs concurrently instead of
            // serializing on a single CPU thread. Null under the same conditions the old shared
            // render_thread_ was null (see use_render_threading_'s doc comment): the inline
            // single-threaded fallback in render_managed_window() applies per window exactly as before.
            unique_ptr<Async::DedicatedThread> render_thread;
            u64 frame_index = 0;
            std::chrono::high_resolution_clock::time_point last_frame_time{};
            f64 last_delta_seconds = 0.0;
        };

        // Waits on every still-in-flight render-thread frame (every window) and empties each ring. Must
        // run before engine_/each window's render_thread start tearing down — Async::DedicatedThread's
        // destructor joins its worker but does not drain queued tasks first (see AffinityImpl.cpp), so
        // any frame still queued at that point would simply be abandoned.
        void drain_render_thread() noexcept;
        // Runs GameLogic shutdown at most once after successful initialization, after all CPU
        // submissions and GPU work are complete but before Engine/device destruction.
        void shutdown_client() noexcept;

        [[nodiscard]] ManagedWindow *find_managed_window(Platform::Windowing::WindowId id) noexcept;
        void process_window_requests();

        // Spawns one window through an explicitly supplied optional provider factory, or SDL3 when
        // factory is null, then registers it with the engine's render-surface set and repaint path.
        // Keeping the factory indirect prevents Application from retaining optional provider symbols.
        bool spawn_managed_window(
            const Platform::Windowing::WindowConfig &config,
            Platform::Windowing::WindowFactory factory,
            bool is_primary);

        // Core per-window render dispatch. `coalesce_if_backpressured` returns false without preparing
        // or queuing work when the render queue is full, so live resize can retain only its newest
        // extent instead of blocking the application or rebuilding every intermediate swapchain.
        [[nodiscard]] bool render_managed_window(ManagedWindow &managed,
                                                 Platform::Windowing::WindowExtent extent,
                                                 bool resized,
                                                 bool coalesce_if_backpressured = false);
        void report_frame_result(const Core::RendererResult &result) noexcept;

        // Updates cached WindowSnapshots from the event channel and publishes them before
        // engine_->update(). This never synchronously queries a live Window: the dedicated event
        // thread can be inside Windows' modal resize loop while the application still needs to tick.
        void sync_window_state(const vector<Platform::Windowing::ManagedWindowEvents> &window_events);

        Platform::Windowing::WindowManager window_manager_{
            Platform::Windowing::WindowManagerPolicy{.event_pump_mode = Platform::Windowing::WindowEventPumpMode::DedicatedEventThread,
                                                     .platform_allows_threads = true}};
        vector<unique_ptr<ManagedWindow>> windows_;
        unique_ptr<Engine> engine_;
        ApplicationClient *client_ = nullptr;
        bool client_initialized_ = false;
        bool client_shutdown_ = false;

        // Decided once in initialize() from RHI::choose_render_threading_mode() and reused every time a
        // window is spawned to decide whether that window's ManagedWindow::render_thread gets a real
        // Async::DedicatedThread or stays null (Web, or the backend/platform declining
        // RHI::RenderThreadingMode above SingleThreaded via render_threading_capabilities()) — null means
        // the inline single-threaded fallback in render_managed_window() applies for that window.
        // Deliberately independent of ApplicationConfig::enable_runtime_window_management: the Renderer/
        // RHI-Vulkan layers are already hardened for two windows rendering concurrently (every lazily-
        // built cache and resource pool in Renderer::Renderer/VulkanRhiDeviceBridge is Async::Mutex- or
        // pool-guarded specifically for this), so docking tear-off windows get real per-window threads
        // like any other window rather than falling back to fully inline rendering.
        bool use_render_threading_ = false;
        u32 max_frames_in_flight_ = 2;
        u32 consecutive_render_errors_ = 0;
    };

} // namespace SFT::Engine
