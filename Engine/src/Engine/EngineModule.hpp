#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <optional>
#include <vector>
#pragma endregion

#include "EcsRendering.hpp"
#include "AssetManager.hpp"
#include <Core/Core.hpp>
#include <Ecs/src/System.hpp>
#include <Ecs/src/World.hpp>
#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>
#include <Renderer/Renderer.hpp>

using std::optional;
using std::vector;

namespace SFT::Engine {

    struct EngineConfig {
        Core::RendererFeatureRequest features{};
        const char *app_name = "Sturdy Engine 5";
        // Walked recursively for *.slang files at the start of Engine::initialize(), before the
        // graphics backend comes up. Relative paths are resolved against the current working directory.
        std::filesystem::path shaders_directory = "Shaders";
    };

    // The glue layer. Owns the high-level Renderer and binds it to Platform windows. The Renderer
    // owns backend lifetimes/synchronization/resource records while still exposing low-level escape
    // hatches for Core backend and RHI access. Future subsystems (audio, physics, scene) hang here.
    class Engine {
      public:
        Engine();
        ~Engine();

        Engine(const Engine &) = delete;
        Engine &operator=(const Engine &) = delete;

        // Bring the renderer up for the first window. The backend constructs and owns the initial
        // render surface, returning an opaque handle for render calls and resize notifications.
        Core::RendererExpected<Core::RenderSurfaceHandle> initialize(Platform::Windowing::Window &window,
                                                                     const EngineConfig &config = {});

        // Adds another window to an already-initialized engine. The backend stores this window's
        // resources (surface, swapchain, ...) keyed by its WindowId, alongside every other window.
        Core::RendererExpected<Core::RenderSurfaceHandle> add_window(Platform::Windowing::Window &window,
                                                                     u32 desired_frames_in_flight = 2);

        // Destroys one window's backend-owned resources. Call when a window is closed.
        void remove_window(Core::RenderSurfaceHandle surface) noexcept;

        // Pairs with Platform::Windowing::Window::recreate(): once the old window has been
        // destroyed and its replacement constructed, call this to retire the old window's
        // backend resources (which are keyed by its now-gone WindowId) and stand up fresh ones
        // for the new window. `old_surface` must not be used again after this call.
        Core::RendererExpected<Core::RenderSurfaceHandle> recreate_window(Core::RenderSurfaceHandle old_surface,
                                                                          Platform::Windowing::Window &new_window,
                                                                          u32 desired_frames_in_flight = 2);

        // Forward a resize-needed notification to the backend. Call on every window resize event,
        // including resize-to-zero (minimized), with the already-resolved framebuffer extent — the
        // backend must not query the window itself (this can be called from a render thread; see
        // EngineBackend::on_surface_resize_needed). The backend rebuilds the swapchain lazily.
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept;

        // Runtime presentation policy for a surface: apps can expose this as vsync / low-latency / tearing
        // options. Changing it marks the swapchain dirty and applies on the next rendered frame.
        Core::RendererResult set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                       const Core::PresentationSettings &settings);

        [[nodiscard]] Core::RendererExpected<Core::RuntimeSettingsChangeResult>
        apply_runtime_settings(Core::RenderSurfaceHandle primary_surface,
                               const EngineConfig &settings);

        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);

        // Runs the ECS render-extraction schedule on the coordinating caller thread and returns an
        // immutable CPU snapshot. Queue this snapshot to the render thread; never hand renderer/RHI
        // objects to ECS workers directly.
        [[nodiscard]] PreparedRenderFrame prepare_render_frame(Core::RenderSurfaceHandle surface,
                                                               const Core::FrameInput &frame,
                                                               const RenderFrameParameters &parameters = {});
        Core::RendererResult render(const PreparedRenderFrame &frame);

        // Consumer extension points. Bind RenderFrameRequests as a World resource, then add
        // read-oriented extraction systems that submit into it.
        [[nodiscard]] Ecs::World &ecs_world() noexcept;
        [[nodiscard]] const Ecs::World &ecs_world() const noexcept;
        [[nodiscard]] Ecs::Schedule &render_extraction_schedule() noexcept;
        [[nodiscard]] RenderFrameRequests &render_frame_requests() noexcept;
        [[nodiscard]] AssetManager &assets() noexcept;
        [[nodiscard]] const AssetManager &assets() const noexcept;

        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept;
        [[nodiscard]] SFT::Renderer::Renderer *renderer() noexcept;
        [[nodiscard]] const SFT::Renderer::Renderer *renderer() const noexcept;
        [[nodiscard]] Core::EngineBackend *graphics_backend() noexcept;
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept;

        // Backend-agnostic info about the GPU in use (name, vendor, driver version, ...). Returns
        // nullopt until a successful initialize() has selected a physical device.
        [[nodiscard]] optional<Core::GpuInfo> gpu_info() const;

        // Shaders discovered and reflected from EngineConfig::shaders_directory during initialize(),
        // before the graphics backend came up. Each one is lazily compiled: target bytecode for an
        // entry point is only generated the first time something asks for it.
        [[nodiscard]] const vector<Core::Slang::UnCompiledShader> &shaders() const noexcept;

        // Block until all in-flight GPU work is complete. The destructor calls this automatically;
        // also useful as an explicit sync point before controlled shutdown or resource reloads.
        void wait_idle() noexcept;

      private:
        SFT::Renderer::Renderer renderer_;
        AssetManager assets_{renderer_};
        Ecs::ComponentRegistry ecs_component_registry_;
        RenderFrameRequests render_frame_requests_{assets_};
        Ecs::World ecs_world_{ecs_component_registry_};
        Ecs::Schedule render_extraction_schedule_;
        Core::RendererCapabilities capabilities_{};
        Core::Slang::ShaderCompiler shader_compiler_;
        vector<Core::Slang::UnCompiledShader> shaders_;
        EngineConfig config_{};
        Platform::Windowing::Window *primary_window_ = nullptr;
        bool initialized_ = false;
    };

} // namespace SFT::Engine
