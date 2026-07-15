#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#pragma endregion

#include <Core/Core.hpp>
#include <Renderer/Renderer.hpp>
#include <RHI/RHI.hpp>
#include <Platform/Platform.hpp>

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
        // including resize-to-zero (minimized). The backend queries extent and rebuilds lazily.
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept;

        // Runtime presentation policy for a surface: apps can expose this as vsync / low-latency / tearing
        // options. Changing it marks the swapchain dirty and applies on the next rendered frame.
        Core::RendererResult set_presentation_settings(Core::RenderSurfaceHandle surface,
                                                       const Core::PresentationSettings &settings);

        [[nodiscard]] Core::RendererExpected<Core::RuntimeSettingsChangeResult>
        apply_runtime_settings(Core::RenderSurfaceHandle primary_surface,
                               const EngineConfig &settings);

        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);

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
        struct DemoSceneResources {
            SFT::Renderer::MaterialTemplateHandle material_template{};
            SFT::Renderer::MaterialInstanceHandle material_instance{};
            SFT::Renderer::MeshHandle floor{};
            SFT::Renderer::MeshHandle sphere{};
            SFT::Renderer::MeshHandle cube{};
            SFT::Renderer::MeshHandle torus{};
            SFT::Renderer::MeshHandle cone{};
            bool ready = false;
        };

        Core::RendererResult create_demo_scene();

        SFT::Renderer::Renderer renderer_;
        Core::RendererCapabilities capabilities_{};
        Core::Slang::ShaderCompiler shader_compiler_;
        vector<Core::Slang::UnCompiledShader> shaders_;
        DemoSceneResources demo_scene_{};
        EngineConfig config_{};
        Platform::Windowing::Window *primary_window_ = nullptr;
        bool initialized_ = false;
    };

} // namespace SFT::Engine
