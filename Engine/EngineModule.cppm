module;

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
    };

    // The glue layer. Owns the renderer backend and binds it to Platform windows: it translates
    // native window handles into API-agnostic surface descriptors (the single place Platform meets
    // Core) and drives per-frame rendering. Future subsystems (audio, physics, scene) hang here.
    class Engine {
      public:
        Engine();
        ~Engine();

        Engine(const Engine &) = delete;
        Engine &operator=(const Engine &) = delete;

        // Bring the renderer up using the first window's surface profile, then create and return
        // the first backend-owned render surface. Call create_surface() for additional windows.
        Core::RendererExpected<Core::RenderSurfaceHandle> initialize(Platform::Windowing::Window &window,
                                                                     const EngineConfig &config = {});

        Core::RendererExpected<Core::RenderSurfaceHandle> create_surface(Platform::Windowing::Window &window,
                                                                         u32 desired_frames_in_flight = 0);
        Core::RendererResult destroy_surface(Core::RenderSurfaceHandle surface);

        // Forward a framebuffer extent change to the backend. Call on every window resize event,
        // including resize-to-zero (minimized). The backend rebuilds the swapchain lazily.
        void on_resize(Core::RenderSurfaceHandle surface, Core::Extent2D extent) noexcept;

        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);

        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept { return capabilities_; }

        // Block until all in-flight GPU work is complete. The destructor calls this automatically;
        // also useful as an explicit sync point before controlled shutdown or resource reloads.
        void wait_idle() noexcept;

      private:
        unique_ptr<Core::EngineBackend> renderer_backend_;
        vector<Core::RenderSurfaceHandle> surfaces_;
        Core::RendererCapabilities capabilities_{};
        EngineConfig config_{};
        bool initialized_ = false;
    };

} // namespace SFT::Engine
