module;

#include <expected>
#include <format>
#include <utility>

export module Sturdy.Engine:EngineImpl;

import :Engine;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.Platform;

using std::format;
using std::unexpected;

namespace SFT::Engine {

    // The API-selection switch point lives behind create_vulkan_backend(): swapping in Metal or
    // WebGPU later is a one-line change here, with no volk/Vulkan headers in this translation unit.
    Engine::Engine()
        : renderer_backend_(Core::create_vulkan_backend()) {
    }

    Engine::~Engine() = default;

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::initialize(Platform::Windowing::Window &window,
                                                                         const EngineConfig &config) {
        if (initialized_) {
            return unexpected(Core::RendererError{Core::RendererErrorCode::OperationFailed,
                                                  "Engine renderer is already initialized."});
        }

        auto wsi_extensions = window.required_vulkan_instance_extensions();
        if (!wsi_extensions) {
            return unexpected(Core::RendererError{
                Core::RendererErrorCode::InitializationFailed,
                format("Failed to query Vulkan WSI extensions from window: {}", wsi_extensions.error().message),
            });
        }

        Core::RendererCreateInfo renderer_info{};
        renderer_info.features = config.features;
        renderer_info.app_name = config.app_name;
        renderer_info.window = &window;
        renderer_info.wsi_extensions = std::move(*wsi_extensions);

        auto surface = renderer_backend_->initialize(renderer_info);
        if (!surface) {
            return unexpected(surface.error());
        }

        initialized_ = true;
        capabilities_ = renderer_backend_->capabilities();
        return *surface;
    }

    void Engine::on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept {
        if (renderer_backend_) {
            renderer_backend_->on_surface_resize_needed(surface);
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
