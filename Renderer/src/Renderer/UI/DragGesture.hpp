#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#pragma endregion

#include <Renderer/UI/Context.hpp>


namespace SFT::UI {

    class DragGestureState {
      public:
        struct UpdateResult {


            bool active = false;


            bool started = false;


            bool ended = false;


            bool committed = false;


            bool cancelled = false;
            glm::vec2 position{0.0f};


            glm::vec2 delta_since_start{0.0f};


            glm::vec2 delta_this_frame{0.0f};
        };


        /// Updates the `DragGestureState` state from the supplied values.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param id Identifier of the target object or resource.
        /// @param threshold `threshold` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        UpdateResult update(Context &ctx, const UString &id, f32 threshold = 0.0f) noexcept;

        /// Reports whether active holds for this `DragGestureState`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_active() const noexcept;
        /// Reports whether capturing holds for this `DragGestureState`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_capturing() const noexcept;
        /// Returns the current or globally available press position value.
        ///
        /// @return Returns the current press position value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] glm::vec2 press_position() const noexcept;


        /// Resets the object to its baseline state.
        ///
        /// @note This function does not throw exceptions.
        void reset() noexcept;


        /// Cancels the outstanding operation when cancellation is still possible.
        ///
        /// @param ctx `ctx` value used by the operation.
        /// @param id Identifier of the target object or resource.
        ///
        /// @note This function does not throw exceptions.
        void cancel(Context &ctx, const UString &id) noexcept;

      private:
        bool capturing_ = false;
        bool started_ = false;
        glm::vec2 press_position_{0.0f};
        glm::vec2 start_position_{0.0f};
        glm::vec2 last_position_{0.0f};
    };

} // namespace SFT::UI
