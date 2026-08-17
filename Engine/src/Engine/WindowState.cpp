#include <Engine/src/Engine/WindowState.hpp>


namespace SFT::Engine {

    /// Performs the sync operation for `Engine` using the supplied arguments.
    ///
    /// @param windows Window used or affected by the operation.
    /// @param primary `primary` value used by the operation.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note This function does not throw exceptions.
    void WindowState::sync(std::vector<WindowSnapshot> windows, std::optional<Platform::Windowing::WindowId> primary) noexcept {
        windows_ = std::move(windows);
        primary_ = primary;
    }

    /// Returns the current or globally available windows value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const WindowSnapshot> WindowState::windows() const noexcept { return windows_; }

    /// Finds the requested entry in the available state.
    ///
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const WindowSnapshot *WindowState::find(Platform::Windowing::WindowId id) const noexcept {
        for (const WindowSnapshot &snapshot : windows_) {
            if (snapshot.id == id) {
                return &snapshot;
            }
        }
        return nullptr;
    }

    /// Returns the current or globally available primary value.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const WindowSnapshot *WindowState::primary() const noexcept {
        return primary_ ? find(*primary_) : nullptr;
    }

} // namespace SFT::Engine

