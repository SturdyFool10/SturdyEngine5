module;

#pragma region Imports
#include <expected>
#include <format>

#include <optional>
#include <utility>
#pragma endregion

module Sturdy.Engine;

import :Engine;
import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.Renderer;
import Sturdy.RHI;
import Sturdy.Platform;

using std::format;

using std::unexpected;

namespace SFT::Engine {

    // The API-selection switch point now lives inside SFT::Renderer::Renderer. Engine owns the
    // high-level renderer, not the raw graphics backend.
    Engine::Engine() = default;

    Engine::~Engine() = default;

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::initialize(Platform::Windowing::Window &window,
                                                                         const EngineConfig &config) {
        if (initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                  "Engine renderer is already initialized."});
        }

        // Reflect every shader on disk before the graphics backend exists, so the rest of startup
        // can see entry points, bindings, and parameter layouts without having generated any
        // target bytecode yet.
        shaders_ = Core::Slang::discover_shaders(config.shaders_directory, shader_compiler_);

        auto wsi_extensions = window.required_vulkan_instance_extensions();
        if (!wsi_extensions) {
            return unexpected(Core::GraphicsBackendError{
                Core::GraphicsBackendErrorCode::InitializationFailed,
                format("Failed to query Vulkan WSI extensions from window: {}", wsi_extensions.error().message),
            });
        }

        Core::RendererCreateInfo renderer_info{};
        renderer_info.features = config.features;
        renderer_info.app_name = config.app_name;
        renderer_info.window = &window;
        renderer_info.wsi_extensions = std::move(*wsi_extensions);
        // Hand the backend the shaders we reflected above; it owns compiling them to its native
        // format. shaders_ outlives this call, so the non-owning span stays valid.
        renderer_info.uncompiled_shaders = shaders_;

        Foundation::log_info("[szdiag] Engine TU sees sizeof(Renderer)={} &renderer_={}",
                             sizeof(SFT::Renderer::Renderer), static_cast<const void *>(&renderer_));
        auto surface = renderer_.initialize(renderer_info);
        if (!surface) {
            return unexpected(surface.error());
        }

        initialized_ = true;
        capabilities_ = renderer_.capabilities();
        return *surface;
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::add_window(Platform::Windowing::Window &window,
                                                                         u32 desired_frames_in_flight) {
        if (!initialized_) {
            return unexpected(Core::GraphicsBackendError{Core::GraphicsBackendErrorCode::OperationFailed,
                                                  "Engine renderer must be initialized before adding another window."});
        }
        return renderer_.create_window_surface(window, desired_frames_in_flight);
    }

    void Engine::remove_window(Core::RenderSurfaceHandle surface) noexcept {
        renderer_.destroy_window_surface(surface);
    }

    Core::RendererExpected<Core::RenderSurfaceHandle> Engine::recreate_window(Core::RenderSurfaceHandle old_surface,
                                                                              Platform::Windowing::Window &new_window,
                                                                              u32 desired_frames_in_flight) {
        remove_window(old_surface);
        return add_window(new_window, desired_frames_in_flight);
    }

    void Engine::on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept {
        renderer_.on_surface_resize_needed(surface);
    }

    Core::RendererResult Engine::render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame) {
        return renderer_.render_frame(surface, frame);
    }

    void Engine::wait_idle() noexcept {
        renderer_.wait_idle();
    }

    std::optional<Core::GpuInfo> Engine::gpu_info() const {
        return renderer_.gpu_info();
    }

    Core::EngineBackend *Engine::graphics_backend() noexcept {
        return renderer_.graphics_backend();
    }

    RHI::RhiDevice *Engine::rhi_device() noexcept {
        return renderer_.rhi_device();
    }

} // namespace SFT::Engine
