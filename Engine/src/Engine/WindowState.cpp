#include <Engine/src/Engine/WindowState.hpp>


namespace SFT::Engine {

    void WindowState::sync(std::vector<WindowSnapshot> windows, std::optional<Platform::Windowing::WindowId> primary) noexcept {
        windows_ = std::move(windows);
        primary_ = primary;
    }

    std::span<const WindowSnapshot> WindowState::windows() const noexcept { return windows_; }

    const WindowSnapshot *WindowState::find(Platform::Windowing::WindowId id) const noexcept {
        for (const WindowSnapshot &snapshot : windows_) {
            if (snapshot.id == id) {
                return &snapshot;
            }
        }
        return nullptr;
    }

    const WindowSnapshot *WindowState::primary() const noexcept {
        return primary_ ? find(*primary_) : nullptr;
    }

} // namespace SFT::Engine

