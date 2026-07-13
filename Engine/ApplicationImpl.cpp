module;

#pragma region Imports
#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>
#pragma endregion

module Sturdy.Engine;

import :Application;
import Sturdy.Foundation;
import Sturdy.Async;
import Sturdy.Core;
import Sturdy.Platform;
import Sturdy.Platform.SDL3;
using std::chrono::duration;
using std::chrono::high_resolution_clock;
using SFT::Foundation::f64;

namespace SFT::Engine {

    Application::Application() = default;
    Application::~Application() = default;

    bool Application::initialize() {
        using namespace Platform::Windowing;

        WindowConfig config{};
        config.title = "Sturdy Engine 5";
        config.extent = {1280, 720};
        config.graphics_api = WindowGraphicsApi::Vulkan;

        // SDL3 is the default windowing backend (broad platform reach + robust Vulkan surface
        // creation). GLFW remains available - this is the one line that selects the backend.
        auto window = Window::create<SDL3::SDL3Window>(config);
        if (!window) {
            Foundation::log_error("Failed to create window: " + window.error().message);
            return false;
        }
        window_ = std::move(*window);

        EngineConfig engine_config{};
        auto surface = engine_.initialize(*window_, engine_config);
        if (!surface) {
            Foundation::log_error("Failed to initialize engine: " + surface.error().message);
            return false;
        }
        surface_ = *surface;

        return true;
    }

    void Application::run() {
        using namespace Platform::Windowing;

        if (!window_ || !surface_) {
            return;
        }

        auto last = high_resolution_clock::now();
        auto last_memory_log = last;
        auto last_title_update = last;
        constexpr f64 memory_log_interval_seconds = 1.0;
        constexpr f64 title_update_interval_seconds = 0.25;
        u64 frame_index = 0;
        u32 consecutive_render_errors = 0;
        usize peak_resident_bytes = 0;

        // Render one frame. Extracted so it can be called both from the normal loop and from
        // the repaint callback that fires during Windows' move/resize modal loop.
        auto render_one_frame = [&]() {
            // Consume a pending resize here rather than only in the main loop below: during
            // Windows' modal move/resize loop this lambda runs as the repaint callback while the
            // main loop is blocked inside DefWindowProc, so this is the only place a resize gets
            // observed mid-drag. Marking the surface dirty makes render_frame rebuild the
            // swapchain at the new size instead of presenting stale-resolution frames.
            if (window_->consume_resize()) {
                engine_.on_surface_resize_needed(*surface_);
            }

            Core::Extent2D framebuffer{};
            if (auto size = window_->framebuffer_size()) {
                framebuffer = {size->x, size->y};
            }
            if (framebuffer.is_zero()) {
                return;
            }
            const auto now = high_resolution_clock::now();
            const f64 delta_seconds = duration<f64>(now - last).count();
            last = now;

            if (duration<f64>(now - last_memory_log).count() >= memory_log_interval_seconds) {
                // Hand back host memory mimalloc is retaining but no longer needs before we
                // sample. mimalloc keeps freed pages committed for fast reuse (delayed purge),
                // so after a startup peak (shader compilation, staging uploads) the reported
                // footprint can look alarmingly large while very little is actually live. Forcing
                // a collect here makes the numbers below reflect what we truly hold.
                Foundation::Memory::collect(true);

                const auto usage = Foundation::Memory::heap_usage();
                // mimalloc's peak_rss is only refreshed during allocator work and can read lower
                // than the live RSS (producing a nonsensical peak < current), so track our own
                // high-water mark from the post-collect samples instead.
                peak_resident_bytes = std::max(peak_resident_bytes, usage.current_resident_bytes);
                Foundation::log_info("Memory usage: resident={} peak_resident={} committed={} peak_committed={}",
                                     Foundation::Memory::format_bytes(usage.current_resident_bytes),
                                     Foundation::Memory::format_bytes(peak_resident_bytes),
                                     Foundation::Memory::format_bytes(usage.current_bytes),
                                     Foundation::Memory::format_bytes(usage.peak_bytes));
                last_memory_log = now;
            }

            if (duration<f64>(now - last_title_update).count() >= title_update_interval_seconds) {
                const std::string title = std::format("SturdyEngine 5 Frame Time: {}, FPS: {:.0f}",
                                                       Foundation::human_readable_time(delta_seconds),
                                                       1.0 / delta_seconds);
                if (auto result = window_->set_title(title.c_str()); !result) {
                    Foundation::log_error("Failed to set window title: {}", result.error().message);
                }
                last_title_update = now;
            }

            if (auto result = engine_.render(*surface_, Core::FrameInput{
                                  .delta_seconds = delta_seconds,
                                  .frame_index = frame_index,
                                  .framebuffer_width = framebuffer.width,
                                  .framebuffer_height = framebuffer.height,
                              }); !result) {
                ++consecutive_render_errors;
                if (consecutive_render_errors == 1 || consecutive_render_errors % 120 == 0) {
                    Foundation::log_error("Render error: " + result.error().message);
                }
            } else {
                consecutive_render_errors = 0;
                ++frame_index;
            }
        };

        // On Windows, dragging the title bar or resizing enters a modal message loop inside
        // DefWindowProc that blocks SDL_PollEvent for the duration of the drag. SDL3 works
        // around this by setting a WM_TIMER on WM_ENTERSIZEMOVE and firing
        // SDL_EVENT_WINDOW_EXPOSED each tick; our repaint callback catches those events via
        // SDL_AddEventWatch and keeps the renderer running on the main thread.
        window_->set_repaint_callback(render_one_frame);

        while (!window_->close_requested()) {
            // Run any work queued via Async::run_on_main_thread() before touching the window/
            // renderer this frame, since that's exactly the kind of main-thread-affined state such
            // jobs exist to touch safely.
            Async::pump_main_thread();

            if (auto pump = window_->pump_events(); !pump) {
                Foundation::log_error("Event pump failed: {}", pump.error().message);
                break;
            }

            bool close_requested = false;
            while (auto event = window_->poll_event()) {
                if (event->kind == WindowEventKind::CloseRequested) {
                    close_requested = true;
                }
            }

            if (close_requested) {
                break;
            }

            // render_one_frame() consumes any pending resize before drawing (see its top), so the
            // rebuild path is shared by both the normal loop and the modal-resize repaint callback.
            render_one_frame();
        }

        window_->set_repaint_callback({});
        engine_.wait_idle();
    }

} // namespace SFT::Engine
