#include <UI/src/UI/DragGesture.hpp>


namespace SFT::UI {

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

    bool DragGestureState::is_active() const noexcept { return capturing_ && started_; }

    bool DragGestureState::is_capturing() const noexcept { return capturing_; }

    glm::vec2 DragGestureState::press_position() const noexcept { return press_position_; }

    void DragGestureState::reset() noexcept {
        capturing_ = false;
        started_ = false;
    }

    void DragGestureState::cancel(Context &ctx, const UString &id) noexcept {
        if (capturing_ && ctx.has_pointer_capture(id)) {
            ctx.release_pointer(id);
        }
        reset();
    }

} // namespace SFT::UI

