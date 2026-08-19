#include <Engine/Application.hpp>


namespace SFT::Engine {

    /// Performs the publish operation for `Engine` using the supplied arguments.
    ///
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void Application::LiveResizeState::publish(WindowManager::WindowExtent extent) noexcept {
        const u64 packed = (static_cast<u64>(extent.x) << 32U) | static_cast<u64>(extent.y);
        pending_extent.store(packed, std::memory_order_release);
    }

    /// Returns the current or globally available consume value.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    /// @note This function does not throw exceptions.
    optional<WindowManager::WindowExtent> Application::LiveResizeState::consume() noexcept {
        const u64 packed = pending_extent.exchange(0, std::memory_order_acq_rel);
        if (packed == 0) {
            return std::nullopt;
        }
        return WindowManager::WindowExtent{
            static_cast<u32>(packed >> 32U),
            static_cast<u32>(packed & 0xFFFFFFFFU),
        };
    }

} // namespace SFT::Engine

