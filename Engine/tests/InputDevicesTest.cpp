#include <Engine/InputState.hpp>

#include <cstdio>

// Touch, pen and gamepad state in `InputState`. Explicit checks rather than assert(): asserts vanish under NDEBUG.

using namespace SFT;
using namespace SFT::Engine;
using WindowManager::PenAxis;
using WindowManager::WindowEventKind;
using WindowManager::WindowPenEvent;
using WindowManager::WindowTouchEvent;

namespace {

    int failures = 0;

    void check(bool condition, const char *what) {
        if (!condition) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    TouchEvent touch(WindowEventKind kind, u64 finger, f32 x, f32 y, f32 dx = 0.0f, f32 dy = 0.0f) {
        return TouchEvent{.kind = kind, .touch = WindowTouchEvent{.device = 1, .finger = finger, .x = x, .y = y, .dx = dx, .dy = dy, .pressure = 0.5f}};
    }

    PenEvent pen(WindowEventKind kind, f32 x = 0.0f, f32 y = 0.0f) {
        return PenEvent{.kind = kind, .pen = WindowPenEvent{.pen = 7, .x = x, .y = y}};
    }

} // namespace

int main() {
    InputState input;

    // -- Touch --
    input.begin_tick();
    input.apply(touch(WindowEventKind::TouchDown, 1, 10.0f, 20.0f));
    input.apply(touch(WindowEventKind::TouchDown, 2, 50.0f, 60.0f));
    check(input.touches().size() == 2, "two fingers are tracked");
    check(input.touches()[0].down && input.touches()[0].just_pressed, "a new finger is down and just pressed");

    input.begin_tick();
    input.apply(touch(WindowEventKind::TouchMotion, 1, 15.0f, 25.0f, 5.0f, 5.0f));
    input.apply(touch(WindowEventKind::TouchMotion, 1, 18.0f, 26.0f, 3.0f, 1.0f));
    check(!input.touches()[0].just_pressed, "'just pressed' lasts one tick");
    check(input.touches()[0].x == 18.0f && input.touches()[0].y == 26.0f, "motion moves the finger");
    check(input.touches()[0].dx == 8.0f && input.touches()[0].dy == 6.0f, "motion accumulates within a tick");

    input.begin_tick();
    check(input.touches()[0].dx == 0.0f, "motion resets each tick");
    input.apply(touch(WindowEventKind::TouchUp, 1, 18.0f, 26.0f));
    input.apply(touch(WindowEventKind::TouchCanceled, 2, 50.0f, 60.0f));
    check(input.touches().size() == 2, "lifted fingers stay listed for their tick");
    check(!input.touches()[0].down && input.touches()[0].just_released && !input.touches()[0].cancelled, "a lift releases the finger");
    check(!input.touches()[1].down && input.touches()[1].just_released && input.touches()[1].cancelled, "a cancel releases it as cancelled");

    input.begin_tick();
    check(input.touches().empty(), "lifted fingers disappear the tick after");

    input.begin_tick();
    input.apply(touch(WindowEventKind::TouchUp, 9, 0.0f, 0.0f));
    check(input.touches().empty(), "a lift for a finger never seen is ignored");

    // -- Pen --
    input.begin_tick();
    input.apply(pen(WindowEventKind::PenProximityIn));
    check(input.pens().size() == 1 && input.pens()[0].in_range, "a pen coming into range is listed");
    input.apply(pen(WindowEventKind::PenDown, 30.0f, 40.0f));
    check(input.pens()[0].down && input.pens()[0].just_down, "the tip touching puts the pen down");
    check(input.pens()[0].x == 30.0f && input.pens()[0].y == 40.0f, "the pen has a position");

    input.begin_tick();
    input.apply(PenEvent{.kind = WindowEventKind::PenAxis, .pen = WindowPenEvent{.pen = 7, .axis = PenAxis::Pressure, .value = 0.75f}});
    input.apply(PenEvent{.kind = WindowEventKind::PenAxis, .pen = WindowPenEvent{.pen = 7, .axis = PenAxis::XTilt, .value = -30.0f}});
    input.apply(PenEvent{.kind = WindowEventKind::PenButtonDown, .pen = WindowPenEvent{.pen = 7, .button = 2}});
    check(!input.pens()[0].just_down, "'just down' lasts one tick");
    check(input.pens()[0].pressure == 0.75f && input.pens()[0].x_tilt == -30.0f, "axes update the pen");
    check(input.pens()[0].buttons == 2u, "barrel button 2 is bit 1");
    input.apply(PenEvent{.kind = WindowEventKind::PenButtonUp, .pen = WindowPenEvent{.pen = 7, .button = 2}});
    check(input.pens()[0].buttons == 0u, "releasing the barrel button clears its bit");

    input.begin_tick();
    input.apply(pen(WindowEventKind::PenProximityOut));
    check(!input.pens()[0].in_range && !input.pens()[0].down && input.pens()[0].just_up, "leaving range lifts a touching pen");
    input.begin_tick();
    check(input.pens().empty(), "a pen that left range disappears the tick after");

    // -- Gamepads: off until asked for, and quiet without hardware --
    check(!input.gamepads_enabled(), "gamepads are off by default");
    input.poll_gamepads();
    check(input.gamepads().empty() && input.gamepad_events().empty(), "no frames while gamepads are off");
    input.set_gamepads_enabled(true);
    input.poll_gamepads();
    check(input.gamepad(0xFFFF'FFF0u) == nullptr, "an unknown gamepad has no frame");
    check(!input.gamepad_button_down(0xFFFF'FFF0u, Gamepads::Button::South), "an unknown gamepad has no buttons down");
    check(input.gamepad_axis(0xFFFF'FFF0u, Gamepads::Axis::LeftX) == 0.0f, "an unknown gamepad has centred axes");

    if (failures == 0) {
        std::printf("InputDevicesTest: passed.\n");
    }
    return failures == 0 ? 0 : 1;
}
