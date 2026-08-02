#pragma once

#include <Foundation/src/Foundation.hpp>

#pragma region Imports
#include <algorithm>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#pragma endregion

#include "Context.hpp"

// Reusable press-capture-drag gesture built directly on Context's own pointer-capture primitives
// (try_capture_pointer/has_pointer_capture/release_pointer, pointer_position/pointer_is_down/
// pointer_pressed_this_frame/pointer_released_this_frame/pointer_cancelled_this_frame — see
// Context.hpp). Slider.hpp and the docking workspace (UI/Docking/DockWorkspace.hpp) both need the
// same press/capture/threshold/release state machine around those primitives; this factors it out
// once so any widget — first-party or an app's own — gets it for free instead of re-deriving it.
// Not a Clay primitive itself (same "not a Context method" reasoning as Button.hpp).
namespace SFT::UI {

    class DragGestureState {
      public:
        struct UpdateResult {
            // The gesture has crossed its start threshold. This remains true on the normal release
            // frame so consumers can apply the pointer's final position before checking `ended`.
            bool active = false;
            // True on exactly the one frame movement first crossed `threshold` — useful for
            // one-shot setup at the moment a drag becomes real (e.g. snapshotting a value to offset
            // future deltas against).
            bool started = false;
            // True on exactly the one frame this gesture concluded, by any means (release, an
            // explicit cancel, or Context dropping orphaned capture — see try_capture_pointer's own
            // doc comment on when that happens). Check `committed`/`cancelled` to tell those apart;
            // `ended && !committed && !cancelled` is an ordinary click that never became a drag.
            bool ended = false;
            // `ended` was a normal release *and* the gesture had genuinely started at some point —
            // mirrors Slider.hpp's own SliderResult::committed.
            bool committed = false;
            // `ended` was a cancellation (Context::pointer_cancelled_this_frame(), e.g. the window
            // lost input focus mid-drag) or capture was found orphaned this frame.
            bool cancelled = false;
            glm::vec2 position{0.0f};
            // Zero until `started`; from then on, position minus the pointer position at which the
            // threshold was crossed. The crossing frame itself therefore reports zero.
            glm::vec2 delta_since_start{0.0f};
            // position minus last frame's position — for widgets that want to accumulate their own
            // running total rather than re-deriving it from delta_since_start each frame.
            glm::vec2 delta_this_frame{0.0f};
        };

        // Call once per frame for the draggable element identified by `id`. A gesture begins the
        // frame `ctx.clicked(id)` fires (the same press-while-hovered edge every button()/click
        // handler in this package already keys off) and this call successfully acquires exclusive
        // pointer capture for `id`; it ends on release, cancellation, or orphaned-capture recovery.
        // `threshold` is how many pixels the pointer must move from the position sampled when this
        // state first observes the press before `started`/`active` report true. Values <= 0 start on
        // the capture frame. A positive value disambiguates an ordinary click from a real drag.
        // Press and release edges latched into the same UI frame are fully resolved by this call:
        // capture never leaks into the next frame.
        UpdateResult update(Context &ctx, const UString &id, f32 threshold = 0.0f) noexcept {
            UpdateResult result{};
            result.position = ctx.pointer_position();

            if (capturing_ && !ctx.has_pointer_capture(id)) {
                // Context drops capture on our behalf if this id wasn't declared on the frame the
                // release would have been observed (e.g. the widget was conditionally hidden).
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

        [[nodiscard]] bool is_active() const noexcept { return capturing_ && started_; }
        [[nodiscard]] bool is_capturing() const noexcept { return capturing_; }
        [[nodiscard]] glm::vec2 press_position() const noexcept { return press_position_; }

        // Clears local gesture bookkeeping without touching Context capture. This is for owners that
        // remove a draggable between frames and therefore cannot safely retain a Context reference;
        // Context drops the now-undeclared element's capture in finish_frame(). Prefer cancel() when
        // the owning Context is available and immediate release is required.
        void reset() noexcept {
            capturing_ = false;
            started_ = false;
        }

        // Forcibly ends the gesture and releases capture without waiting for a pointer event — for
        // a widget that's about to be destroyed/hidden mid-drag and can't wait for next frame's
        // orphan recovery.
        void cancel(Context &ctx, const UString &id) noexcept {
            if (capturing_ && ctx.has_pointer_capture(id)) {
                ctx.release_pointer(id);
            }
            reset();
        }

      private:
        bool capturing_ = false;
        bool started_ = false;
        glm::vec2 press_position_{0.0f};
        glm::vec2 start_position_{0.0f};
        glm::vec2 last_position_{0.0f};
    };

} // namespace SFT::UI
