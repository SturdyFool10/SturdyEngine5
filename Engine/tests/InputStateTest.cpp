#include <Engine/InputState.hpp>

#include <cassert>

int main() {
    using namespace SFT::Engine;
    using namespace SFT::Platform::Windowing;

    InputState input;


    input.begin_tick();
    input.apply(KeyboardEvent{.key_code = KeyboardKey::Space, .action = ButtonAction::Pressed});
    input.apply(KeyboardEvent{.key_code = KeyboardKey::LeftShift, .action = ButtonAction::Pressed});
    assert(input.key_down(KeyboardKey::Space));
    assert(input.key_just_pressed(KeyboardKey::Space));
    assert(!input.key_just_released(KeyboardKey::Space));
    assert(input.key_down(KeyboardKey::LeftShift));
    assert(has_modifier(input.modifiers(), KeyModifiers::Shift));
    assert(!has_modifier(input.modifiers(), KeyModifiers::Control));
    assert(!input.key_down(KeyboardKey::A));


    input.begin_tick();
    assert(input.key_down(KeyboardKey::Space));
    assert(!input.key_just_pressed(KeyboardKey::Space));
    assert(!input.key_just_released(KeyboardKey::Space));
    assert(has_modifier(input.modifiers(), KeyModifiers::Shift));


    input.begin_tick();
    input.apply(KeyboardEvent{.key_code = KeyboardKey::Space, .action = ButtonAction::Released});
    input.apply(KeyboardEvent{.key_code = KeyboardKey::A, .action = ButtonAction::Pressed});
    assert(!input.key_down(KeyboardKey::Space));
    assert(input.key_just_released(KeyboardKey::Space));
    assert(!input.key_just_pressed(KeyboardKey::Space));
    assert(input.key_down(KeyboardKey::A));
    assert(input.key_just_pressed(KeyboardKey::A));



    input.begin_tick();
    input.apply(KeyboardEvent{.key_code = KeyboardKey::LeftShift, .action = ButtonAction::Released});
    assert(!input.key_down(KeyboardKey::LeftShift));
    assert(!has_modifier(input.modifiers(), KeyModifiers::Shift));


    input.begin_tick();
    input.apply(MouseButtonEvent{.mouse = {.button_code = MouseButton::Extra9}, .action = ButtonAction::Pressed});
    assert(input.mouse_down(MouseButton::Extra9));
    assert(input.mouse_just_pressed(MouseButton::Extra9));
    assert(!input.mouse_down(MouseButton::Left));



    input.begin_tick();
    input.apply(MouseMoveEvent{.mouse = {.x = 10.0f, .y = 20.0f, .delta_x = 1.0f, .delta_y = 2.0f}});
    input.apply(MouseMoveEvent{.mouse = {.x = 11.0f, .y = 22.0f, .delta_x = 1.0f, .delta_y = 2.0f}});
    assert(input.mouse_x() == 11.0f && input.mouse_y() == 22.0f);
    assert(input.mouse_delta_x() == 2.0f && input.mouse_delta_y() == 4.0f);
    input.begin_tick();
    assert(input.mouse_delta_x() == 0.0f && input.mouse_delta_y() == 0.0f);


    input.begin_tick();
    input.apply(TextInputEvent{.text = {.utf8 = "H"}});
    input.apply(TextInputEvent{.text = {.utf8 = "i"}});
    assert(input.text_this_tick() == "Hi");
    input.begin_tick();
    assert(input.text_this_tick().empty());



    assert(!input.composing());
    assert(input.composition_text().empty());
    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = "k"}});
    assert(input.composing());
    assert(input.composition_text() == "k");

    input.begin_tick();
    assert(input.composing());
    assert(input.composition_text() == "k");

    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = "こんにちは"}});
    assert(input.composing());
    assert(input.composition_text() == "こんにちは");


    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = ""}});
    assert(!input.composing());
    assert(input.composition_text().empty());


    input.begin_tick();
    input.apply(TextEditingEvent{.text = {.utf8 = "konnichiwa"}});
    assert(input.composing());
    input.apply(TextInputEvent{.text = {.utf8 = "こんにちは"}});
    assert(!input.composing());
    assert(input.composition_text().empty());
    assert(input.text_this_tick() == "こんにちは");

    return 0;
}
