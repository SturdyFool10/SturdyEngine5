#pragma once

#include "Core/EngineBackend.hpp"
#include "Core/Renderer.hpp"
#include "Platform/Window/Window.hpp"

#include <memory>
#include <vector>

namespace SFT::Engine {

    struct EngineConfig {
        Core::RendererFeatureRequest features{};
        const char *app_name = "Sturdy Engine 5";
    };

    // The glue. Owns the renderer backend and binds it to a window: it translates the window's
    // native handles into an API-agnostic surface descriptor (the single place Platform meets
    // Core) and drives per-frame rendering. Future subsystems (audio, physics, scene) will hang
    // off this same coordinator.
    class Engine {
      public:
        Engine();
        ~Engine();

        Engine(const Engine &) = delete;
        Engine &operator=(const Engine &) = delete;

        // Bring the renderer up using the first window's surface profile, then create and return
        // the first backend-owned render surface. Additional windows can call create_surface().
        Core::RendererExpected<Core::RenderSurfaceHandle> initialize(Platform::Windowing::Window &window, const EngineConfig &config = {});

        Core::RendererExpected<Core::RenderSurfaceHandle> create_surface(Platform::Windowing::Window &window, u32 desired_frames_in_flight = 0);
        Core::RendererResult destroy_surface(Core::RenderSurfaceHandle surface);
        Core::RendererResult recreate_surface(Core::RenderSurfaceHandle surface, Platform::Windowing::Window &window, u32 desired_frames_in_flight = 0);

        void on_resize(Core::RenderSurfaceHandle surface, Core::Extent2D extent);
        Core::RendererResult render(Core::RenderSurfaceHandle surface, const Core::FrameInput &frame);

        [[nodiscard]] const Core::RendererCapabilities &capabilities() const noexcept { return capabilities_; }

        void wait_idle(Core::RenderSurfaceHandle surface) noexcept;
        void wait_idle() noexcept;

      private:
        std::unique_ptr<Core::EngineBackend> renderer_backend_;
        std::vector<Core::RenderSurfaceHandle> surfaces_;
        Core::RendererCapabilities capabilities_{};
        EngineConfig config_{};
        bool initialized_ = false;
    };

} // namespace SFT::Engine
