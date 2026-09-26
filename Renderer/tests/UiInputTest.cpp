/// UI::UiInput accumulates input from any source and hands a frame of it to the UI; end_frame() clears the
/// one-frame edges but keeps held state. UI::ui_uv_at_ray / ui_pixel_from_uv turn a ray at a world panel into the
/// pointer position UiInput wants.

#include <Renderer/UI/UI.hpp>

#include <cmath>
#include <iostream>
#include <string>

namespace {
    using namespace SFT;
    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }
    bool near(f32 a, f32 b) { return std::abs(a - b) < 1.0e-4f; }
} // namespace

int main() {
    {
        UI::UiInput input;
        input.move_pointer({10.0f, 20.0f});
        input.set_pointer_down(true);
        check(input.pointer().down && input.pointer().pressed, "a press sets down and the pressed edge");
        check(input.pointer().press_position && input.pointer().press_position->x == 10.0f, "a press remembers where it began");
        input.move_pointer({50.0f, 60.0f});
        input.add_scroll({0.0f, 2.0f});
        input.add_scroll({0.0f, 3.0f});
        check(input.pointer().scroll_delta.y == 5.0f, "scroll accumulates within a frame");
        input.end_frame();
        check(input.pointer().down && !input.pointer().pressed && !input.pointer().press_position, "end_frame clears the edge but keeps the button held");
        check(input.pointer().scroll_delta == glm::vec2{0.0f}, "end_frame clears scroll");
        check(input.pointer().position.x == 50.0f, "the pointer position persists across frames");
        input.set_pointer_down(false);
        check(!input.pointer().down && input.pointer().released, "a release sets the released edge");
        input.set_pointer_down(true);
        input.cancel_pointer();
        check(!input.pointer().down && input.pointer().cancelled, "cancel drops the button and flags cancellation");
    }
    {
        UI::UiInput input;
        input.add_text("ab");
        input.add_text("é");
        input.press_key(UI::EditKey::Backspace);
        input.set_shift_held(true);
        auto text = input.text_input();
        check(text.typed_text == "abé" && text.keys.size() == 1 && text.shift_held, "typed text, keys and modifiers reach the frame's text input");
        input.set_composition("か");
        check(input.text_input().composing && input.text_input().composition_text == "か", "IME pre-edit is reported while composing");
        input.add_text("漢");
        check(!input.text_input().composing, "committing text ends the composition");
        input.end_frame();
        text = input.text_input();
        check(text.typed_text.empty() && text.keys.empty(), "end_frame clears typed text and key presses");
        check(text.shift_held, "held modifiers survive end_frame");
    }
    {
        // A 2 x 1 panel facing +z, top-left at (-1, 0.5, 0): u to the right, v down.
        const UI::UiWorldPlane plane{.origin = {-1.0f, 0.5f, 0.0f}, .u_axis = {2.0f, 0.0f, 0.0f}, .v_axis = {0.0f, -1.0f, 0.0f}};
        const glm::vec3 eye{0.0f, 0.0f, 4.0f};
        auto center = UI::ui_uv_at_ray(plane, eye, {0.0f, 0.0f, -1.0f});
        check(center && near(center->x, 0.5f) && near(center->y, 0.5f), "a ray through the middle lands at uv (0.5, 0.5)");
        auto corner = UI::ui_uv_at_ray(plane, eye, glm::vec3{-1.0f, 0.5f, 0.0f} - eye);
        check(corner && near(corner->x, 0.0f) && near(corner->y, 0.0f), "a ray at the top-left corner lands at uv (0, 0)");
        auto scaled = UI::ui_uv_at_ray(plane, eye, (glm::vec3{0.5f, -0.25f, 0.0f} - eye) * 3.0f);
        check(scaled && near(scaled->x, 0.75f) && near(scaled->y, 0.75f), "the ray direction need not be normalised");
        check(!UI::ui_uv_at_ray(plane, eye, glm::vec3{3.0f, 0.0f, 0.0f} - eye), "a ray past the panel's edge misses");
        auto outside = UI::ui_uv_at_ray(plane, eye, glm::vec3{3.0f, 0.0f, 0.0f} - eye, true);
        check(outside && outside->x > 1.0f, "allow_outside reports the coordinate beyond the edge (for drags that leave the panel)");
        check(!UI::ui_uv_at_ray(plane, eye, {0.0f, 0.0f, 1.0f}), "a ray pointing away from the panel misses");
        check(!UI::ui_uv_at_ray(plane, eye, {1.0f, 0.0f, 0.0f}), "a ray parallel to the panel misses");
        const glm::vec2 pixel = UI::ui_pixel_from_uv(*center, {800.0f, 400.0f});
        check(near(pixel.x, 400.0f) && near(pixel.y, 200.0f), "uv maps to the UI's pixel extent");

        // A panel tilted 90 degrees (facing +x) hit from the side, with a skewed basis.
        const UI::UiWorldPlane side{.origin = {0.0f, 1.0f, -1.0f}, .u_axis = {0.0f, 0.0f, 2.0f}, .v_axis = {0.0f, -2.0f, 0.0f}};
        auto hit = UI::ui_uv_at_ray(side, {5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
        check(hit && near(hit->x, 0.5f) && near(hit->y, 0.5f), "a panel in any orientation maps rays into its own uv space");
    }
    return failures == 0 ? 0 : 1;
}
