#pragma once

#include <Foundation/src/Foundation.hpp>

#include <Engine/Engine.hpp>

#include <type_traits>

namespace SFT::ApplicationHost {

    /// Drive an already-constructed product client. This overload lets Runtime inject a GameLogic
    /// factory/configuration while a future Editor can construct its own richer client policy.
    [[nodiscard]] i32 run(
        Engine::ApplicationClient &client,
        const Foundation::CliArgs &args);

    /// Convenience overload for default-constructible product clients. Platform entrypoints remain
    /// product-owned; this helper only centralizes the Engine::Application driving loop.
    ///
    /// CLI argument parsing itself (Foundation::CliArgs/args_from_argv/args_from_windows_command_line)
    /// lives in Foundation, not here — it has no dependency on Engine or the client-hosting loop, so
    /// any consumer (including one that never wants Engine at all) can reuse it directly.
    template <class Client>
    [[nodiscard]] i32 run(const Foundation::CliArgs &args) {
        static_assert(std::is_base_of_v<Engine::ApplicationClient, Client>,
                      "ApplicationHost::run<Client>() requires an Engine::ApplicationClient.");
        static_assert(std::is_default_constructible_v<Client>,
                      "ApplicationHost::run<Client>() constructs Client itself and needs a default constructor.");
        Client client;
        return run(client, args);
    }

} // namespace SFT::ApplicationHost
