#include <Platform/src/Platform/Window/Window.hpp>
#include "Window.hpp"

#include <tracy/Tracy.hpp>

namespace SFT::Platform::Windowing::Detail {

WindowId allocate_window_id() noexcept {
            ZoneScopedN("Windowing::Detail::allocate_window_id");
            static std::atomic<usize> next_id{0};
            return static_cast<WindowId>(next_id.fetch_add(1, std::memory_order_relaxed));
        }

} // namespace SFT::Platform::Windowing::Detail

namespace SFT::Platform::Windowing {

Window::Window(ConstructorKey) noexcept
            : id_(Detail::allocate_window_id()) {}

[[nodiscard]] WindowId Window::id() const noexcept { return id_; }

expected<void, WindowError> Window::lock_mouse_to_window() noexcept {
            ZoneScopedN("Window::lock_mouse_to_window");
            return set_mouse_locked(true);
        }

expected<void, WindowError> Window::unlock_mouse() noexcept {
            ZoneScopedN("Window::unlock_mouse");
            return set_mouse_locked(false);
        }

void Window::set_live_resize_callback(std::function<void(WindowExtent)>             ) noexcept {}

} // namespace SFT::Platform::Windowing

namespace SFT::Platform::Windowing {

    optional<WindowHdrProperties> Window::hdr_properties() const noexcept {
        return std::nullopt;
    }

    expected<WindowPosition, WindowError> Window::global_cursor_position() const noexcept {
        return unexpected(WindowError{
            WindowErrorCode::Unsupported,
            "Global cursor position is unavailable for this window provider.",
        });
    }

    expected<void, WindowError> Window::start_text_input() noexcept { return {}; }

    expected<void, WindowError> Window::stop_text_input() noexcept { return {}; }

    expected<void, WindowError> Window::set_text_input_area(TextInputArea         ) noexcept { return {}; }

} // namespace SFT::Platform::Windowing

