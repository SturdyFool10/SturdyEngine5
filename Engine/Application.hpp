#pragma once

#include "Engine/Engine.hpp"
#include "Platform/Window/Window.hpp"

#include <memory>
#include <optional>

using std::optional;
using std::unique_ptr;

namespace SFT::Engine {

    // Process host: owns the window and the engine, runs the main loop, and forwards OS events,
    // resizes and frame timing into the engine. This is the boundary where the platform/OS lives;
    // everything below Engine is platform- and API-agnostic.
    class Application {
      public:
        Application();
        ~Application();

        bool initialize();
        void run();

      private:
        unique_ptr<Platform::Windowing::Window> window_;
        optional<Core::RenderSurfaceHandle> surface_;
        Engine engine_;
    };

} // namespace SFT::Engine
