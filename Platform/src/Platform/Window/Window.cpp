#include "Window.hpp"

namespace SFT::Platform::Windowing::Detail {

WindowId allocate_window_id() noexcept {
            static std::atomic<usize> next_id{0};
            return static_cast<WindowId>(next_id.fetch_add(1, std::memory_order_relaxed));
        }

} // namespace SFT::Platform::Windowing::Detail

namespace SFT::Platform::Windowing {

Window::Window(ConstructorKey) noexcept
            : id_(Detail::allocate_window_id()) {}

[[nodiscard]] WindowId Window::id() const noexcept { return id_; }

expected<void, WindowError> Window::lock_mouse_to_window() noexcept {
            return set_mouse_locked(true);
        }

expected<void, WindowError> Window::unlock_mouse() noexcept {
            return set_mouse_locked(false);
        }

void Window::set_repaint_callback(std::function<void()> /*callback*/) noexcept {}

} // namespace SFT::Platform::Windowing
