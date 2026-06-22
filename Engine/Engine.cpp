#include "Engine/Engine.hpp"

#include <string>

import Sturdy.Foundation;

namespace SFT::Engine {

    namespace {

        Core::SurfaceSystem to_surface_system(Platform::Windowing::NativeWindowSystem system) noexcept {
            using NS = Platform::Windowing::NativeWindowSystem;
            switch (system) {
                case NS::Win32:
                    return Core::SurfaceSystem::Win32;
                case NS::X11:
                    return Core::SurfaceSystem::X11;
                case NS::Wayland:
                    return Core::SurfaceSystem::Wayland;
                case NS::Cocoa:
                    return Core::SurfaceSystem::Cocoa;
                default:
                    return Core::SurfaceSystem::Unknown;
            }
        }

    } // namespace

    // The API-selection switch point lives behind create_vulkan_backend(): swapping in a Metal
    // or WebGPU backend later is a one-line change here, with no volk/Vulkan headers in Engine.
    Engine::Engine()
        : renderer_backend_(Core::create_vulkan_backend()) {
    }

    Engine::~Engine() = default;

    Core::RendererResult Engine::initialize(Platform::Windowing::Window &window, const EngineConfig &config) {
        window_ = &window;

        const auto native = window.native_window_handle();
        Core::RenderSurfaceDescriptor surface{};
        surface.system = to_surface_system(native.system);
        surface.display = native.display;
        surface.window = native.window;
        if (window.backend_kind() == Platform::Windowing::WindowBackendKind::SDL3) {
            surface.sdl_window = window.native_backend_handle();
        }

        Core::Extent2D extent{};
        if (auto framebuffer = window.framebuffer_size()) {
            extent = {framebuffer->x, framebuffer->y};
        } else {
            return Core::renderer_error(Core::RendererErrorCode::InitializationFailed,
                                        "Failed to query framebuffer size: " + framebuffer.error().message);
        }

        Core::RendererInit init{};
        init.surface = surface;
        init.framebuffer_extent = extent;
        init.features = config.features;
        init.app_name = config.app_name;

        // Two-phase bring-up: device-level core first (no surface needed), then bind the
        // window surface + swapchain. Capabilities are valid once both have run.
        if (auto result = renderer_backend_->initialize(); !result) {
            return result;
        }
        if (auto result = renderer_backend_->bind_surface(init); !result) {
            return result;
        }

        capabilities_ = renderer_backend_->capabilities();
        return {};
    }

    void Engine::on_resize(Core::Extent2D extent) {
        if (auto result = renderer_backend_->on_resize(extent); !result) {
            Foundation::log_warn("Renderer resize failed: " + result.error().message);
        }
    }

    Core::RendererResult Engine::render(const Core::FrameInput &frame) {
        return renderer_backend_->render_frame(frame);
    }

    void Engine::wait_idle() noexcept {
        if (renderer_backend_) {
            renderer_backend_->wait_idle();
        }
    }

} // namespace SFT::Engine
