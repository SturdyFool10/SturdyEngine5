#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Engine/Engine.hpp>

namespace SFT::Runtime {

    /// Standalone-product policy owned by Runtime rather than by consumer GameLogic. A future Editor
    /// drives the same GameLogic with its own windows, titles, viewports, and update cadence.
    struct RuntimeConfig {
        Engine::ApplicationConfig application{};
        UString primary_window_title{UString{"Sturdy application"}};
    };

    /// Construct one GameLogic session through an explicit factory and run it as a standalone desktop
    /// application. No demo/game implementation is linked unless its factory symbol is passed here.
    [[nodiscard]] i32 run(
        const Foundation::CliArgs &args,
        RuntimeConfig config,
        Engine::GameLogicFactory game_logic_factory);

} // namespace SFT::Runtime
