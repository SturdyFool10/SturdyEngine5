#include <UI/src/UI/DragGesture.hpp>


namespace SFT::UI {

    /// Updates the `UI` state from the supplied values.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    /// @param threshold `threshold` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note This function does not throw exceptions.
    DragGestureState::UpdateResult DragGestureState::update(Context &ctx, const UString &id, f32 threshold) noexcept {
        UpdateResult result{};
        result.position = ctx.pointer_position();

        if (capturing_ && !ctx.has_pointer_capture(id)) {


            result.ended = true;
            result.cancelled = true;
            reset();
            return result;
        }

        if (!capturing_) {
            if (!ctx.clicked(id) || !ctx.try_capture_pointer(id)) {
                return result;
            }
            capturing_ = true;
            started_ = false;
            press_position_ = result.position;
            start_position_ = press_position_;
            last_position_ = press_position_;
        }

        if (ctx.pointer_cancelled_this_frame()) {
            result.ended = true;
            result.cancelled = true;
            ctx.release_pointer(id);
            reset();
            return result;
        }

        const f32 effective_threshold = std::max(threshold, 0.0f);
        if (!started_ && glm::length(result.position - press_position_) >= effective_threshold) {
            started_ = true;
            start_position_ = result.position;
            result.started = true;
        }
        if (started_) {
            result.active = true;
            result.delta_since_start = result.position - start_position_;
            result.delta_this_frame = result.position - last_position_;
        }
        last_position_ = result.position;

        if (ctx.pointer_released_this_frame() ||
            (!ctx.pointer_is_down() && !ctx.pointer_pressed_this_frame())) {
            result.ended = true;
            result.committed = started_;
            ctx.release_pointer(id);
            reset();
        }
        return result;
    }

    /// Reports whether active holds for this `UI`.
    ///
    /// @return Returns the current is active value.
    /// @note This function does not throw exceptions.
    bool DragGestureState::is_active() const noexcept { return capturing_ && started_; }

    /// Reports whether capturing holds for this `UI`.
    ///
    /// @return Returns the current is capturing value.
    /// @note This function does not throw exceptions.
    bool DragGestureState::is_capturing() const noexcept { return capturing_; }

    /// Returns the current or globally available press position value.
    ///
    /// @return Returns the current press position value.
    /// @note This function does not throw exceptions.
    glm::vec2 DragGestureState::press_position() const noexcept { return press_position_; }

    /// Resets the object to its baseline state.
    ///
    /// @return Returns the current reset value.
    /// @note This function does not throw exceptions.
    void DragGestureState::reset() noexcept {
        capturing_ = false;
        started_ = false;
    }

    /// Cancels the outstanding operation when cancellation is still possible.
    ///
    /// @param ctx `ctx` value used by the operation.
    /// @param id Identifier of the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void DragGestureState::cancel(Context &ctx, const UString &id) noexcept {
        if (capturing_ && ctx.has_pointer_capture(id)) {
            ctx.release_pointer(id);
        }
        reset();
    }

} // namespace SFT::UI

