#pragma once

#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <Async/Async.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <vector>
#pragma endregion

#include "EngineModule.hpp"
#include <Core/Core.hpp>
#include <Platform/Platform.hpp>

using std::optional;
using std::unique_ptr;
using std::vector;

namespace SFT::Engine {

    struct ApplicationConfig {
        Platform::Windowing::WindowConfig primary_window;
        EngineConfig engine;
        // No periodic title mutation unless the consumer explicitly enables it.
        optional<f64> primary_window_title_update_interval_seconds;
    };

    struct ApplicationFrameStats {
        f64 frame_seconds = 0.0;
        u64 frame_index = 0;
        usize window_count = 0;
    };

    struct ApplicationError {
        UString message;
    };

    using ApplicationResult = std::expected<void, ApplicationError>;

    // Host-facing lifecycle boundary. Runtime is the first simulated API consumer: it owns policy
    // and sample/game behavior while Application owns only platform pumping and render dispatch.
    class ApplicationClient {
      public:
        virtual ~ApplicationClient() = default;

        [[nodiscard]] virtual const ApplicationConfig &application_config() const noexcept = 0;
        [[nodiscard]] virtual ApplicationResult on_engine_initialized(Engine &engine) = 0;
        [[nodiscard]] virtual UString primary_window_title(
            Engine &engine,
            const ApplicationFrameStats &stats) = 0;
        [[nodiscard]] virtual optional<RenderFrameParameters> request_render_frame(
            Engine &engine,
            Core::RenderSurfaceHandle surface,
            const Core::FrameInput &frame) = 0;
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
            std::atomic<bool> resize_pending{false};
            std::deque<Async::TaskHandle<Core::RendererResult>> in_flight_frames;
            optional<Async::TaskHandle<void>> remove_surface_task;
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
        // run before engine_/render_thread_ start tearing down — Async::DedicatedThread's destructor
        // joins its worker but does not drain queued tasks first (see AffinityImpl.cpp), so any frame
        // still queued at that point would simply be abandoned.
        void drain_render_thread() noexcept;

        [[nodiscard]] ManagedWindow *find_managed_window(Platform::Windowing::WindowId id) noexcept;

        // Spawns one SDL3 window and registers it with both WindowManager and the engine's
        // render-surface set, wiring up its repaint callback. Kept out of the exported module
        // interface to avoid exporting a large backend-specific template/lambda body through the C++
        // module BMI; Clang ThinLTO was miscompiling that boundary in Dist.
        bool spawn_sdl3_managed_window(const Platform::Windowing::WindowConfig &config, bool is_primary);

        // Core per-window render dispatch: given this tick's already-known framebuffer extent and
        // whether a resize was just observed, builds FrameInput and either runs it inline or dispatches
        // it onto render_thread_. Deliberately touches only `managed` and `engine_` — never
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

        Platform::Windowing::WindowManager window_manager_{
            Platform::Windowing::WindowManagerPolicy{.event_pump_mode = Platform::Windowing::WindowEventPumpMode::CallerThread,
                                                     .platform_allows_threads = false}};
        vector<unique_ptr<ManagedWindow>> windows_;
        unique_ptr<Engine> engine_;
        ApplicationClient *client_ = nullptr;

        // Primary Render Thread: null means the single-threaded fallback is in effect (Web, or the
        // backend/platform declining RHI::RenderThreadingMode above SingleThreaded via
        // render_threading_capabilities()) and render calls happen inline on the caller thread. Non-null
        // means every RHI/Vulkan call after initialize() happens on this dedicated thread instead.
        unique_ptr<Async::DedicatedThread> render_thread_;
        u32 max_frames_in_flight_ = 2;
        u32 consecutive_render_errors_ = 0;
    };

} // namespace SFT::Engine
