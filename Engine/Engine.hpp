#pragma once

#include "Core/EngineBackend.hpp"
#include "Core/Renderer.hpp"
#include "Platform/Window/Window.hpp"

#include <memory>

namespace SFT::Engine {

    struct EngineConfig {
        Core::RendererFeatureRequest features {};
        const char* app_name = "Sturdy Engine 5";
    };

    // The glue. Owns the renderer backend and binds it to a window: it translates the window's
    // native handles into an API-agnostic surface descriptor (the single place Platform meets
    // Core) and drives per-frame rendering. Future subsystems (audio, physics, scene) will hang
    // off this same coordinator.
    class Engine {
    public:
        Engine();
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        // Bring the renderer up against the given window. Reads the window's native handle +
        // framebuffer size and hands them to the backend. Call once after the window exists.
        Core::RendererResult initialize(Platform::Windowing::Window& window, const EngineConfig& config = {});

        void on_resize(Core::Extent2D extent);
        Core::RendererResult render(const Core::FrameInput& frame);

        [[nodiscard]] const Core::RendererCapabilities& capabilities() const noexcept { return capabilities_; }

        void wait_idle() noexcept;

    private:
        std::unique_ptr<Core::EngineBackend> renderer_backend_;
        Platform::Windowing::Window* window_ = nullptr; // non-owning; owned by Application
        Core::RendererCapabilities capabilities_ {};
    };

} // namespace SFT::Engine
