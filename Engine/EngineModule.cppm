module;

#include <memory>

export module Sturdy.Engine:Engine;

import Sturdy.Foundation;
import Sturdy.Core;
import Sturdy.Platform;

using std::unique_ptr;

export namespace SFT::Engine {

    struct EngineConfig {
        Core::RendererFeatureRequest features{};
        const char *app_name = "Sturdy Engine 5";
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

        // Forward a resize-needed notification to the backend. Call on every window resize event,
        // including resize-to-zero (minimized). The backend queries extent and rebuilds lazily.
        void on_surface_resize_needed(Core::RenderSurfaceHandle surface) noexcept;

        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);

        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept { return capabilities_; }

        // Block until all in-flight GPU work is complete. The destructor calls this automatically;
        // also useful as an explicit sync point before controlled shutdown or resource reloads.
        void wait_idle() noexcept;

      private:
        unique_ptr<Core::EngineBackend> renderer_backend_;
        Core::RendererCapabilities capabilities_{};
        bool initialized_ = false;
    };

} // namespace SFT::Engine
