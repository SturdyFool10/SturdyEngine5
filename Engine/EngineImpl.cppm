module;

#include <expected>
#include <string>
#include <vector>

export module Sturdy.Engine:EngineImpl;

import :Engine;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.Platform;

using std::string;
using std::unexpected;

namespace SFT::Engine {

    namespace {

        Core::SurfaceSystem to_surface_system(Platform::Windowing::NativeWindowSystem system) noexcept {
            using NS = Platform::Windowing::NativeWindowSystem;
            switch (system) {
                case NS::Win32:    return Core::SurfaceSystem::Win32;
                case NS::X11:      return Core::SurfaceSystem::X11;
                case NS::Wayland:  return Core::SurfaceSystem::Wayland;
                case NS::Cocoa:    return Core::SurfaceSystem::Cocoa;
                default:           return Core::SurfaceSystem::Unknown;
            }
        }

        Core::SurfaceProvider to_surface_provider(Platform::Windowing::WindowBackendKind kind) noexcept {
            using BK = Platform::Windowing::WindowBackendKind;
            switch (kind) {
                case BK::SDL3: return Core::SurfaceProvider::SDL3;
                case BK::GLFW: return Core::SurfaceProvider::GLFW;
            }
            return Core::SurfaceProvider::Unknown;
        }

        Core::RendererExpected<Core::RenderSurfaceCreateInfo> surface_create_info_from_window(
            Platform::Windowing::Window &window,
            u32 desired_frames_in_flight) {

            const auto native = window.native_window_handle();
            if (!native) {
                return unexpected(Core::RendererError{
                    Core::RendererErrorCode::InitializationFailed,
                    "Failed to query native window handle: " + native.error().message,
                });
            }

            const auto provider_window = window.native_backend_handle();
            if (!provider_window) {
                return unexpected(Core::RendererError{
                    Core::RendererErrorCode::InitializationFailed,
                    "Failed to query native backend handle: " + provider_window.error().message,
                });
            }

            const auto framebuffer = window.framebuffer_size();
            if (!framebuffer) {
                return unexpected(Core::RendererError{
                    Core::RendererErrorCode::InitializationFailed,
                    "Failed to query framebuffer size: " + framebuffer.error().message,
                });
            }

            Core::RenderSurfaceCreateInfo info{};
            info.descriptor.provider        = to_surface_provider(window.backend_kind());
            info.descriptor.system          = to_surface_system(native->system);
            info.descriptor.display         = native->display;
            info.descriptor.window          = native->window;
            info.descriptor.provider_window = *provider_window;
            info.framebuffer_extent         = {framebuffer->x, framebuffer->y};
            info.desired_frames_in_flight   = desired_frames_in_flight == 0 ? 2u : desired_frames_in_flight;
            return info;
        }

    } // namespace

    // The API-selection switch point lives behind create_vulkan_backend(): swapping in Metal or
    // WebGPU later is a one-line change here, with no volk/Vulkan headers in this translation unit.
    Engine::Engine()
        : renderer_backend_(Core::create_vulkan_backend()) {
    }

    Engine::~Engine() {
        if (!renderer_backend_) {
            return;
        }
        // Destroy surfaces in reverse order. destroy_surface() drains per-surface in-flight work
        // before releasing GPU resources. The backend destructor then calls wait_idle() and
        // releases the device and instance — no explicit wait needed here.
        for (auto it = surfaces_.rbegin(); it != surfaces_.rend(); ++it) {
            if (auto result = renderer_backend_->destroy_surface(*it); !result) {
                Foundation::log_warn("Failed to destroy render surface: {}", result.error().message);
            }
        }
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::initialize(Platform::Windowing::Window &window,
                                                                         const EngineConfig &config) {
        if (initialized_) {
            return unexpected(Core::RendererError{Core::RendererErrorCode::OperationFailed,
                                                  "Engine renderer is already initialized."});
        }

        config_ = config;

        auto surface_info = surface_create_info_from_window(window, config_.features.desired_frames_in_flight);
        if (!surface_info) {
            return unexpected(surface_info.error());
        }

        Core::RendererCreateInfo renderer_info{};
        renderer_info.features                  = config_.features;
        renderer_info.app_name                  = config_.app_name;
        renderer_info.initial_surface_provider  = surface_info->descriptor.provider;
        renderer_info.initial_surface_system    = surface_info->descriptor.system;

        if (auto result = renderer_backend_->initialize(renderer_info); !result) {
            return unexpected(result.error());
        }

        initialized_ = true;
        capabilities_ = renderer_backend_->capabilities();

        auto surface = renderer_backend_->create_surface(*surface_info);
        if (!surface) {
            return unexpected(surface.error());
        }

        surfaces_.push_back(*surface);
        return *surface;
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::create_surface(Platform::Windowing::Window &window,
                                                                              u32 desired_frames_in_flight) {
        if (!initialized_) {
            return unexpected(Core::RendererError{Core::RendererErrorCode::InitializationFailed,
                                                  "Engine renderer must be initialized before creating a render surface."});
        }

        const u32 frames = desired_frames_in_flight == 0 ? config_.features.desired_frames_in_flight
                                                         : desired_frames_in_flight;
        auto surface_info = surface_create_info_from_window(window, frames);
        if (!surface_info) {
            return unexpected(surface_info.error());
        }

        auto surface = renderer_backend_->create_surface(*surface_info);
        if (!surface) {
            return unexpected(surface.error());
        }

        surfaces_.push_back(*surface);
        return *surface;
    }

    Core::RendererResult Engine::destroy_surface(Core::RenderSurfaceHandle surface) {
        if (!renderer_backend_) {
            return Core::renderer_error(Core::RendererErrorCode::OperationFailed, "Renderer backend is unavailable.");
        }

        if (auto result = renderer_backend_->destroy_surface(surface); !result) {
            return result;
        }

        std::erase(surfaces_, surface);
        return {};
    }

    void Engine::on_resize(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept {
        if (renderer_backend_) {
            renderer_backend_->on_resize(surface, extent);
        }
    }

    Core::RendererResult Engine::render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame) {
        if (!renderer_backend_) {
            return Core::renderer_error(Core::RendererErrorCode::OperationFailed, "Renderer backend is unavailable.");
        }
        return renderer_backend_->render_frame(surface, frame);
    }

    void Engine::wait_idle() noexcept {
        if (renderer_backend_) {
            renderer_backend_->wait_idle();
        }
    }

} // namespace SFT::Engine
