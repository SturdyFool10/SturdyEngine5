#pragma once

#include <Foundation/Foundation.hpp>

#include <Engine/Engine.hpp>

#include <type_traits>

namespace SFT::ApplicationHost {


    /// Runs the requested work.
    ///
    /// @param client `client` value used by the operation.
    /// @param args `args` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] i32 run(
        Engine::ApplicationClient &client,
        const Foundation::CliArgs &args);


    /// Runs the requested work.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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
