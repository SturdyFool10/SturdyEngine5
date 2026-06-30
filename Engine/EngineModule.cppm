module;

#include <filesystem>
#include <memory>
#include <vector>

export module Sturdy.Engine:Engine;

import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.Platform;

using std::unique_ptr;
using std::vector;

export namespace SFT::Engine {

    struct EngineConfig {
        Core::RendererFeatureRequest features{};
        const char *app_name = "Sturdy Engine 5";
        // Walked recursively for *.slang files at the start of Engine::initialize(), before the
        // graphics backend comes up. Relative paths are resolved against the current working directory.
        std::filesystem::path shaders_directory = "Shaders";
    };

    // The glue layer. Owns the renderer backend and binds it to Platform windows: the backend owns,
    // constructs, resizes, and destroys all render surfaces. Future subsystems (audio, physics,
    // scene) hang here.
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

        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);

        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept { return capabilities_; }

        // Shaders discovered and reflected from EngineConfig::shaders_directory during initialize(),
        // before the graphics backend came up. Each one is lazily compiled: target bytecode for an
        // entry point is only generated the first time something asks for it.
        [[nodiscard]] const vector<Core::Slang::UnCompiledShader> &shaders() const noexcept { return shaders_; }

        // Block until all in-flight GPU work is complete. The destructor calls this automatically;
        // also useful as an explicit sync point before controlled shutdown or resource reloads.
        void wait_idle() noexcept;

      private:
        unique_ptr<Core::EngineBackend> renderer_backend_;
        Core::RendererCapabilities capabilities_{};
        Core::Slang::ShaderCompiler shader_compiler_;
        vector<Core::Slang::UnCompiledShader> shaders_;
        bool initialized_ = false;
    };

} // namespace SFT::Engine
