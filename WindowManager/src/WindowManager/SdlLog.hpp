#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <SDL3/SDL.h>

#include <mutex>
#pragma endregion

namespace SFT::WindowManager::Detail {

    /// Sends everything SDL logs through `Foundation::log*` instead of SDL's own stderr output, so an application that registered a
    /// log sink or turned console logging off sees (or silences) SDL's messages the same way as the engine's. SDL's verbose, debug
    /// and info priorities are not problem reports and take the engine's lowest severity; warnings and errors keep theirs.
    /// Safe to call repeatedly and from every place that initializes an SDL subsystem; only the first call installs the hook.
    ///
    /// @note This function does not throw exceptions.
    inline void route_sdl_logging_to_foundation() noexcept {
        static std::once_flag installed;
        std::call_once(installed, [] {
            SDL_SetLogOutputFunction(
                [](void *, int category, SDL_LogPriority priority, const char *message) {
                    const char *text = message != nullptr ? message : "";
                    switch (priority) {
                        case SDL_LOG_PRIORITY_WARN:
                            Foundation::log_warn("SDL[{}]: {}", category, text);
                            break;
                        case SDL_LOG_PRIORITY_ERROR:
                        case SDL_LOG_PRIORITY_CRITICAL:
                            Foundation::log_error("SDL[{}]: {}", category, text);
                            break;
                        default:
                            Foundation::log_trace("SDL[{}]: {}", category, text);
                            break;
                    }
                },
                nullptr);
        });
    }

} // namespace SFT::WindowManager::Detail
