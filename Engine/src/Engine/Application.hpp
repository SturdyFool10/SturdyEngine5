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
#include <Core/Core.hpp>
#include <Platform/Platform.hpp>

using std::optional;
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
        // Per-window render bookkeeping — everything render_managed_window() needs that isn't shared
        // app-wide state. Held by pointer in windows_ so the vector can grow (spawning more windows at
        // runtime) without invalidating references captured by an in-flight render task or a repaint
        // callback.
        struct ManagedWindow {
            Platform::Windowing::WindowId window_id{};
            optional<Core::RenderSurfaceHandle> surface;
            bool primary = false;
            bool closing = false;
            // Latched from WindowEventKind::FocusGained/FocusLost — Window exposes no direct getter
            // for this, only the one-shot event (see WindowState.hpp).
            bool focused = false;
            std::atomic<bool> resize_pending{false};
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
            // Last time a wait_for_completion=true (Windows interactive-drag) repaint actually
            // rendered, as opposed to being throttle-skipped — see render_managed_window's
            // wait_for_completion doc for why this throttle exists.
            std::chrono::high_resolution_clock::time_point last_synchronous_repaint_time{};
            // How long that last synchronous repaint actually took to render+present. Feeds the
            // adaptive fallback in render_managed_window: an unusually slow one (GPU/driver hiccup,
            // not something the engine controls — see the wait_for_completion doc) means the *next*
            // repaint skips forcing a synchronous wait, so a slow driver can't freeze the OS's own
            // move/resize loop for a second multi-second stretch back to back.
            f64 last_synchronous_repaint_duration_seconds = 0.0;
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

        // Core per-window render dispatch: given this tick's already-known framebuffer extent and
        // whether a resize was just observed, builds FrameInput and either runs it inline or dispatches
        // it onto managed.render_thread. Deliberately touches only `managed` and `engine_` — never
        // window_manager_ — since it also runs from inside a Window's repaint callback, which itself
        // fires from inside WindowManager's own dispatch(); re-entering dispatch() from there would
        // deadlock a single-worker DedicatedThread waiting on itself.
        //
        // wait_for_completion forces this call to block until the frame it just queued has actually
        // finished (rather than only enforcing the usual max_frames_in_flight_ backpressure), so the
        // window's on-screen content is fully caught up before returning. Only the Windows-only
        // interactive-move/resize repaint path (see SDL3Window::sdl_repaint_watch) needs this: that
        // path is driven synchronously from inside SDL's blocked modal move/resize pump, and without
        // it the render thread free-runs ahead as fast as WM_PAINT fires while swapchain recreation
        // (tens to thousands of ms each) can't keep up, so the visible content lags the drag by however
        // large that backlog has grown instead of tracking it live.
        //
        // wait_for_completion also applies two guards, both scoped to just this synchronous path —
        // neither ever touches the pipelined async path used everywhere else:
        //
        //  1. A small minimum-interval throttle (min_synchronous_repaint_interval_seconds in the
        //     .cpp) pacing how often it *starts* a new synchronous render. Windows can dispatch
        //     WM_PAINT during a drag far faster than a full render+swapchain-recreate can complete;
        //     the throttle keeps that from turning into a wall of redundant back-to-back rebuilds.
        //  2. An adaptive fallback: if the *previous* synchronous repaint measured unusually slow
        //     (see last_synchronous_repaint_duration_seconds on ManagedWindow), this call skips
        //     forcing the wait and dispatches through the normal pipelined path instead. Direct
        //     instrumentation traced an observed multi-second-per-call stall to the GPU driver's
        //     vkCreateSwapchainKHR itself — correlated with a preceding idle gap, recovering
        //     immediately after, the signature of a GPU power-state wake-up rather than anything
        //     this engine's own bookkeeping controls. When the driver does stall like that, forcing
        //     every subsequent repaint to also block would turn one hardware hiccup into the OS's
        //     entire move/resize loop being frozen for its whole duration; falling back lets the
        //     drag stay interactive (content trailing briefly) instead.
        //
        // The extent used once a throttled or recovering call finally renders synchronously again is
        // still whatever is current at that moment, so it stays visually live rather than stale.
        // The normal per-frame call from run()'s main loop (used on every platform, including Windows
        // outside of an active drag) keeps the default pipelined behavior for throughput.
        void render_managed_window(ManagedWindow &managed, Platform::Windowing::WindowExtent extent, bool resized, bool wait_for_completion = false);
        void report_frame_result(const Core::RendererResult &result) noexcept;

        // Latches each managed window's focused state from this tick's events, then builds a fresh
        // WindowSnapshot per window (querying the live Window on this, the window-owning thread) and
        // publishes them to engine_->window_state(). Called once per run() tick, before
        // engine_->update() runs any schedule that might read Ecs::ReadResource<WindowState>.
        void sync_window_state(const vector<Platform::Windowing::ManagedWindowEvents> &window_events);

        Platform::Windowing::WindowManager window_manager_{
            Platform::Windowing::WindowManagerPolicy{.event_pump_mode = Platform::Windowing::WindowEventPumpMode::CallerThread,
                                                     .platform_allows_threads = false}};
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
