#pragma once

#include <Ecs/Resource.hpp>
#include <WindowManager/WindowManager.hpp>

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace SFT::Engine {


    struct WindowSnapshot {
        WindowManager::WindowId id{};
        /// Window-coordinate size: the space mouse events, `position` and IME rectangles use. Equal to
        /// `framebuffer_size` on Win32/X11, smaller on macOS/scaled Wayland/Web.
        WindowManager::WindowExtent size{};
        /// Physical pixels; what rendering and the swapchain use.
        WindowManager::WindowExtent framebuffer_size{};
        WindowManager::WindowPosition position{};
        /// The OS UI scale for the display the window is on (1.0 = 100%). Independent of
        /// `pixel_density()`: Windows at 200% reports 2.0 here with a density of 1.
        f32 content_scale = 1.0f;
        f32 opacity = 1.0f;
        bool mouse_locked = false;


        bool focused = false;
        /// The window's OS-level handle, captured when the window is created. Empty when the
        /// platform could not provide one.
        std::optional<WindowManager::NativeWindowHandle> native_handle;

        /// Physical pixels per window-coordinate unit (see `WindowManager::pixel_density`).
        ///
        /// @return `framebuffer_size / size`, or 1 when either is empty.
        [[nodiscard]] glm::vec2 pixel_density() const noexcept { return WindowManager::pixel_density(size, framebuffer_size); }

        /// Converts a window-coordinate point (e.g. a mouse event position) into framebuffer pixels.
        ///
        /// @param window_point Point in window coordinates.
        ///
        /// @return The same point in physical pixels.
        [[nodiscard]] glm::vec2 window_to_framebuffer(glm::vec2 window_point) const noexcept { return window_point * pixel_density(); }

        /// Converts a framebuffer-pixel point into window coordinates.
        ///
        /// @param framebuffer_point Point in physical pixels.
        ///
        /// @return The same point in window coordinates.
        [[nodiscard]] glm::vec2 framebuffer_to_window(glm::vec2 framebuffer_point) const noexcept { return framebuffer_point / pixel_density(); }
    };


    class WindowState {
      public:
        /// Performs the sync operation for `WindowState` using the supplied arguments.
        ///
        /// @param windows Window used or affected by the operation.
        /// @param primary `primary` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void sync(std::vector<WindowSnapshot> windows, std::optional<WindowManager::WindowId> primary) noexcept;

        /// Returns the current or globally available windows value.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::span<const WindowSnapshot> windows() const noexcept;

        /// Finds the requested entry in the available state.
        ///
        /// @param id Identifier of the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const WindowSnapshot *find(WindowManager::WindowId id) const noexcept;

        /// Returns the current or globally available primary value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const WindowSnapshot *primary() const noexcept;

      private:
        std::vector<WindowSnapshot> windows_;
        std::optional<WindowManager::WindowId> primary_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::WindowState, "sturdy.engine.window_state");
