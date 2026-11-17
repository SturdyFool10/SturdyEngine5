#pragma once

namespace SFT::WindowManager::Detail {

    /// Loads libwayland-client at run time (the engine's Wayland code is not linked against it, so a machine without a Wayland client
    /// library can still run the program) and binds every function and core interface the window-effect code uses. Idempotent and
    /// thread-safe. Under a Wayland window SDL3 has already loaded the same library, so this just takes another reference to it.
    ///
    /// @return `true` when every `wl_*` symbol resolved; `false` when the library is missing or lacks one. Call before the first `wl_*`
    ///         call; the Wayland-effect entry points report "unavailable" on `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool wayland_client_available() noexcept;

} // namespace SFT::WindowManager::Detail
