#pragma once

#include <Ecs/src/Resource.hpp>
#include <Platform/Platform.hpp>

#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace SFT::Engine {


    struct WindowSnapshot {
        Platform::Windowing::WindowId id{};
        Platform::Windowing::WindowExtent size{};
        Platform::Windowing::WindowExtent framebuffer_size{};
        Platform::Windowing::WindowPosition position{};
        f32 opacity = 1.0f;
        bool mouse_locked = false;


        bool focused = false;
    };


    class WindowState {
      public:
        /// Performs the sync operation for `WindowState` using the supplied arguments.
        ///
        /// @param windows Window used or affected by the operation.
        /// @param primary `primary` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void sync(std::vector<WindowSnapshot> windows, std::optional<Platform::Windowing::WindowId> primary) noexcept;

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
        [[nodiscard]] const WindowSnapshot *find(Platform::Windowing::WindowId id) const noexcept;

        /// Returns the current or globally available primary value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const WindowSnapshot *primary() const noexcept;

      private:
        std::vector<WindowSnapshot> windows_;
        std::optional<Platform::Windowing::WindowId> primary_;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::WindowState, "sturdy.engine.window_state");
