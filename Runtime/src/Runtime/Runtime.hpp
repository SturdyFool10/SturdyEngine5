#pragma once

#include <Foundation/Foundation.hpp>

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

    /// Like `run`, for a game-logic object that already exists, so it can carry state a bare factory function cannot (for
    /// example the callbacks of a foreign-language module). Pass an `Engine::CompositeGameLogic` to run several modules,
    /// written in different languages, in one process.
    ///
    /// @param args Command-line arguments.
    /// @param config Application and window configuration.
    /// @param game_logic The game logic; null logs a diagnostic and returns 1.
    ///
    /// @return The process exit status.
    [[nodiscard]] i32 run_logic(
        const Foundation::CliArgs &args,
        RuntimeConfig config,
        std::unique_ptr<Engine::GameLogic> game_logic);

} // namespace SFT::Runtime
