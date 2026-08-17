#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Engine/Engine.hpp>

namespace SFT::Runtime {


    struct RuntimeConfig {
        Engine::ApplicationConfig application{};
        UString primary_window_title{UString{"Sturdy application"}};
    };


    /// Runs the requested work.
    ///
    /// @param args `args` value used by the operation.
    /// @param config Configuration values controlling the operation.
    /// @param game_logic_factory `game_logic_factory` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] i32 run(
        const Foundation::CliArgs &args,
        RuntimeConfig config,
        Engine::GameLogicFactory game_logic_factory);

} // namespace SFT::Runtime
