#pragma once

#include "Platform/Window/Window.hpp"
#include "Engine/Engine.hpp"

#include <memory>

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
        std::unique_ptr<Platform::Windowing::Window> window_;
        Engine engine_;
    };

} // namespace SFT::Engine
