module;

#include <chrono>
#include <format>
#include <string>
#include <utility>

export module Sturdy.Engine:ApplicationImpl;

import :Application;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.Platform;
import Sturdy.Platform.SDL3;

using std::chrono::duration;
using std::chrono::high_resolution_clock;

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

        auto surface = engine_.initialize(*window_);
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
        u64 frame_index = 0;

        while (!window_->close_requested()) {
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

            // Notify the renderer that surface-sized resources may need rebuilding.
            if (window_->consume_resize()) {
                engine_.on_surface_resize_needed(*surface_);
            }

            if (close_requested) {
                break;
            }

            const auto now = high_resolution_clock::now();
            const f64 delta_seconds = duration<f64>(now - last).count();
            last = now;

            const std::string title = std::format("SturdyEngine 5 Frame Time: {}", delta_seconds);
            if (auto result = window_->set_title(title.c_str()); !result) {
                Foundation::log_error("Failed to set window title: {}", result.error().message);
            }

            // Skip rendering while minimized (zero-area framebuffer).
            Core::Extent2D framebuffer{};
            if (auto size = window_->framebuffer_size()) {
                framebuffer = {size->x, size->y};
            }
            if (framebuffer.is_zero()) {
                continue;
            }

            if (auto result = engine_.render(*surface_, Core::FrameInput{delta_seconds, frame_index}); !result) {
                Foundation::log_error("Render error: " + result.error().message);
                break;
            }
            ++frame_index;
        }

        engine_.wait_idle();
    }

} // namespace SFT::Engine
