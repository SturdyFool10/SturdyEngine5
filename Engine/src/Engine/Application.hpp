#pragma once

#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <vector>
#include <Async/Async.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <Platform/Platform.hpp>
#include "EngineModule.hpp"

using std::optional;
using std::unique_ptr;
using std::vector;

namespace SFT::Engine {

    // Process host: owns the WindowManager and the engine, runs the main loop, and forwards OS events,
    // resizes and frame timing into the engine for every managed window. This is the boundary where the
    // platform/OS lives; everything below Engine is platform- and API-agnostic.
    class Application {
      public:
        Application();
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
        void render_managed_window(ManagedWindow &managed, Platform::Windowing::WindowExtent extent, bool resized);
        void report_frame_result(const Core::RendererResult &result) noexcept;

        Platform::Windowing::WindowManager window_manager_{
            Platform::Windowing::WindowManagerPolicy{.event_pump_mode = Platform::Windowing::WindowEventPumpMode::CallerThread,
                                                      .platform_allows_threads = false}};
        vector<unique_ptr<ManagedWindow>> windows_;
        unique_ptr<Engine> engine_;

        // Primary Render Thread: null means the single-threaded fallback is in effect (Web, or the
        // backend/platform declining RHI::RenderThreadingMode above SingleThreaded via
        // render_threading_capabilities()) and render calls happen inline on the caller thread. Non-null
        // means every RHI/Vulkan call after initialize() happens on this dedicated thread instead.
        unique_ptr<Async::DedicatedThread> render_thread_;
        u32 max_frames_in_flight_ = 2;
        u32 consecutive_render_errors_ = 0;
    };

} // namespace SFT::Engine
