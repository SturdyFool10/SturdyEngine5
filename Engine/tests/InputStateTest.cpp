#include <Engine/InputState.hpp>

#include <cassert>

int main() {
    using namespace SFT::Engine;
    using namespace SFT::Platform::Windowing;

    InputState input;

    // Tick 1: press Space and LeftShift.
    input.begin_tick();
    input.apply(KeyboardEvent{.key_code = KeyboardKey::Space, .action = ButtonAction::Pressed});
    input.apply(KeyboardEvent{.key_code = KeyboardKey::LeftShift, .action = ButtonAction::Pressed});
    assert(input.key_down(KeyboardKey::Space));
    assert(input.key_just_pressed(KeyboardKey::Space));
    assert(!input.key_just_released(KeyboardKey::Space));
    assert(input.key_down(KeyboardKey::LeftShift));
    assert(has_modifier(input.modifiers(), KeyModifiers::Shift));
    assert(!has_modifier(input.modifiers(), KeyModifiers::Control));
    assert(!input.key_down(KeyboardKey::A)); // never pressed

    // Tick 2: nothing new happens — Space/LeftShift stay held, just_pressed must clear.
    input.begin_tick();
    assert(input.key_down(KeyboardKey::Space));
    assert(!input.key_just_pressed(KeyboardKey::Space)); // cleared by begin_tick(), still held though
    assert(!input.key_just_released(KeyboardKey::Space));
    assert(has_modifier(input.modifiers(), KeyModifiers::Shift)); // still held from tick 1

    // Tick 3: release Space, press A (a plain, non-modifier key) at the same time.
    input.begin_tick();
    input.apply(KeyboardEvent{.key_code = KeyboardKey::Space, .action = ButtonAction::Released});
    input.apply(KeyboardEvent{.key_code = KeyboardKey::A, .action = ButtonAction::Pressed});
    assert(!input.key_down(KeyboardKey::Space));
    assert(input.key_just_released(KeyboardKey::Space));
    assert(!input.key_just_pressed(KeyboardKey::Space));
    assert(input.key_down(KeyboardKey::A));
    assert(input.key_just_pressed(KeyboardKey::A));

    // Modifier keys are ordinary KeyboardKey values — same pressed-state machinery as any other key,
    // not special-cased — and released the same way.
    input.begin_tick();
    input.apply(KeyboardEvent{.key_code = KeyboardKey::LeftShift, .action = ButtonAction::Released});
    assert(!input.key_down(KeyboardKey::LeftShift));
    assert(!has_modifier(input.modifiers(), KeyModifiers::Shift));

    // Mouse buttons: same pressed/just_pressed/just_released machinery, keyed by MouseButton.
    input.begin_tick();
    input.apply(MouseButtonEvent{.mouse = {.button_code = MouseButton::Extra9}, .action = ButtonAction::Pressed});
    assert(input.mouse_down(MouseButton::Extra9));
    assert(input.mouse_just_pressed(MouseButton::Extra9));
    assert(!input.mouse_down(MouseButton::Left));

    // Mouse deltas accumulate within one tick (a high-polling-rate device can deliver several move
    // events before the next tick folds them in) and reset at the next begin_tick().
    input.begin_tick();
    input.apply(MouseMoveEvent{.mouse = {.x = 10.0f, .y = 20.0f, .delta_x = 1.0f, .delta_y = 2.0f}});
    input.apply(MouseMoveEvent{.mouse = {.x = 11.0f, .y = 22.0f, .delta_x = 1.0f, .delta_y = 2.0f}});
    assert(input.mouse_x() == 11.0f && input.mouse_y() == 22.0f);
    assert(input.mouse_delta_x() == 2.0f && input.mouse_delta_y() == 4.0f);
    input.begin_tick();
    assert(input.mouse_delta_x() == 0.0f && input.mouse_delta_y() == 0.0f);

    // Text input: OS-composed UTF-8, accumulated across events this tick, cleared next tick.
    input.begin_tick();
    input.apply(TextInputEvent{.text = {.utf8 = "H"}});
    input.apply(TextInputEvent{.text = {.utf8 = "i"}});
    assert(input.text_this_tick() == "Hi");
    input.begin_tick();
    assert(input.text_this_tick().empty());

    // IME composition (preedit): unlike text_this_tick(), this persists across ticks where nothing
    // changed — a composition can sit open for many frames while the user thinks.
    assert(!input.composing());
    assert(input.composition_text().empty());
    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = "k"}});
    assert(input.composing());
    assert(input.composition_text() == "k");
    // A later tick with no new TextEditingEvent must not lose the composition.
    input.begin_tick();
    assert(input.composing());
    assert(input.composition_text() == "k");
    // Composition updates as the IME keeps converting romaji.
    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = "こんにちは"}});
    assert(input.composing());
    assert(input.composition_text() == "こんにちは");
    // Per SDL3's documented contract, an empty composition-text event means editing finished
    // (cancelled here, rather than confirmed) — composing() must go false, not just the text.
    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = ""}});
    assert(!input.composing());
    assert(input.composition_text().empty());
    // A commit (TextInputEvent) also always ends whatever composition preceded it, even without an
    // intervening empty TextEditingEvent — an IME's confirm keystroke doesn't send both.
    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = "konnichiwa"}});
    assert(input.composing());
    input.apply(TextInputEvent{.text = {.utf8 = "こんにちは"}});
    assert(!input.composing());
    assert(input.composition_text().empty());
    assert(input.text_this_tick() == "こんにちは");

    return 0;
}
