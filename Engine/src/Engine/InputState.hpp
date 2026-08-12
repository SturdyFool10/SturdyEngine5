#pragma once

#include "EcsEvents.hpp"

#include <Ecs/src/Resource.hpp>
#include <Platform/Platform.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Engine {

    // Ordinary World resource: accumulated per-tick input state — the read side of "is this key or
    // mouse button currently down," which nothing in the engine could answer before this
    // (plans/ecs-engine-subsystem-access.md's Phase 2, "InputState"). `Engine` owns the persistent
    // instance (`EngineModule.hpp`), folded in place every tick by a built-in system
    // (`EngineImpl.cpp`) that reads the same typed `KeyboardEvent`/`TextInputEvent`/`MouseMoveEvent`/
    // `MouseButtonEvent`/`MouseWheelEvent` streams any other consumer already can (`EcsEvents.hpp`) —
    // never rebound, so a `Ecs::ReadResource<InputState>` a system holds mid-tick is never invalidated
    // by the update itself (same contract `WindowState`/`RenderFrameRequests` already rely on).
    // `Ecs::ReadResource<InputState>` only — input flows one direction (OS -> Application -> ECS).
    //
    // Two distinct paths, pick deliberately:
    //   - `key_down()`/`key_just_pressed()`/`key_just_released()` — raw scancode-driven, fires on the
    //     physical key regardless of layout/IME, with no OS key-repeat applied (repeat is still
    //     available directly off the raw `KeyboardEvent` stream via `.repeat`, for a system that wants
    //     it). Lowest latency; for game actions and rebindable controls, including modifier keys —
    //     `LeftShift`/`RightShift`/etc. are ordinary `KeyboardKey` values here, not special-cased.
    //   - `text_this_tick()` — OS-composed UTF-8 text (`WindowTextInputEvent`), already IME/layout-
    //     aware and already repeat-consistent with the OS's own key-repeat rate/delay for free, since
    //     the OS is what generates the repeated `TextInput` events in the first place. For actual
    //     typing — text fields, chat, console input.
    //   - `modifiers()` is a coarse, derived-on-demand convenience (any-side Shift/Control/Alt/Super,
    //     plus Caps/Num lock) for the common "is Shift held" check — it reads the same per-key state
    //     `key_down()` does, not a second, driftable source of truth.
    class InputState {
      public:
        // Clears just_pressed/just_released/text_this_tick/this-tick deltas — called once per tick,
        // before this tick's events are folded in via apply().
        void begin_tick() noexcept {
            std::fill(key_just_pressed_.begin(), key_just_pressed_.end(), false);
            std::fill(key_just_released_.begin(), key_just_released_.end(), false);
            std::fill(mouse_just_pressed_.begin(), mouse_just_pressed_.end(), false);
            std::fill(mouse_just_released_.begin(), mouse_just_released_.end(), false);
            text_this_tick_.clear();
            mouse_delta_x_ = 0.0f;
            mouse_delta_y_ = 0.0f;
            wheel_delta_x_ = 0.0f;
            wheel_delta_y_ = 0.0f;
        }

        void apply(const KeyboardEvent &event) noexcept {
            if (event.key_code == KeyboardKey::Unknown) {
                return;
            }
            const usize index = static_cast<usize>(event.key_code);
            if (index >= key_count) {
                return;
            }
            if (event.pressed()) {
                if (!key_pressed_[index]) {
                    key_just_pressed_[index] = true;
                }
                key_pressed_[index] = true;
            } else {
                key_pressed_[index] = false;
                key_just_released_[index] = true;
            }
        }

        void apply(const TextInputEvent &event) noexcept {
            // WindowTextInputEvent::utf8 is a zero-initialized fixed buffer; providers write a NUL-
            // terminated sequence into it, but strnlen keeps this bounded even if that ever weren't
            // true rather than trusting the buffer blindly.
            const usize length = strnlen(event.text.utf8, sizeof(event.text.utf8));
            text_this_tick_.append(event.text.utf8, length);
            // A commit always ends whatever composition preceded it (an IME's own Enter/Space to
            // confirm a conversion never arrives as a *second*, separate event) — see composing()'s
            // own doc comment for why this can't be inferred from composition_text() alone.
            composing_ = false;
            composition_text_.clear();
        }

        void apply(const TextEditingEvent &event) noexcept {
            const usize length = strnlen(event.text.utf8, sizeof(event.text.utf8));
            // Per SDL3's documented TextEditing contract, empty composition text means the edit is
            // finished (confirmed or cancelled) and nothing further should be shown — the same rule
            // this engine's InputState applies here rather than needing a separate "ended" event.
            composing_ = length != 0;
            composition_text_.assign(event.text.utf8, length);
        }

        void apply(const MouseMoveEvent &event) noexcept {
            mouse_x_ = event.mouse.x;
            mouse_y_ = event.mouse.y;
            // Accumulate, not overwrite: a high-polling-rate mouse can deliver many move events in
            // one tick, and every one of them contributes real motion this tick's consumers need.
            mouse_delta_x_ += event.mouse.delta_x;
            mouse_delta_y_ += event.mouse.delta_y;
        }

        void apply(const MouseButtonEvent &event) noexcept {
            if (event.mouse.button_code == Platform::Windowing::MouseButton::Unknown) {
                return;
            }
            const usize index = static_cast<usize>(event.mouse.button_code);
            if (index >= mouse_button_count) {
                return;
            }
            if (event.action == ButtonAction::Pressed) {
                if (!mouse_pressed_[index]) {
                    mouse_just_pressed_[index] = true;
                }
                mouse_pressed_[index] = true;
            } else {
                mouse_pressed_[index] = false;
                mouse_just_released_[index] = true;
            }
        }

        void apply(const MouseWheelEvent &event) noexcept {
            wheel_delta_x_ += event.wheel.x;
            wheel_delta_y_ += event.wheel.y;
        }

        [[nodiscard]] bool key_down(KeyboardKey key) const noexcept { return get(key_pressed_, key); }
        [[nodiscard]] bool key_just_pressed(KeyboardKey key) const noexcept { return get(key_just_pressed_, key); }
        [[nodiscard]] bool key_just_released(KeyboardKey key) const noexcept { return get(key_just_released_, key); }

        [[nodiscard]] Platform::Windowing::KeyModifiers modifiers() const noexcept {
            using Platform::Windowing::KeyModifiers;
            KeyModifiers mods = KeyModifiers::None;
            if (key_down(KeyboardKey::LeftShift) || key_down(KeyboardKey::RightShift)) {
                mods |= KeyModifiers::Shift;
            }
            if (key_down(KeyboardKey::LeftControl) || key_down(KeyboardKey::RightControl)) {
                mods |= KeyModifiers::Control;
            }
            if (key_down(KeyboardKey::LeftAlt) || key_down(KeyboardKey::RightAlt)) {
                mods |= KeyModifiers::Alt;
            }
            if (key_down(KeyboardKey::LeftSuper) || key_down(KeyboardKey::RightSuper)) {
                mods |= KeyModifiers::Super;
            }
            if (key_down(KeyboardKey::CapsLock)) {
                mods |= KeyModifiers::CapsLock;
            }
            if (key_down(KeyboardKey::NumLock)) {
                mods |= KeyModifiers::NumLock;
            }
            return mods;
        }

        [[nodiscard]] std::string_view text_this_tick() const noexcept { return text_this_tick_; }

        // Unlike text_this_tick() (a per-tick delta, cleared every begin_tick()), these describe
        // ongoing IME state and persist across ticks where nothing changed — a composition can sit
        // idle for many frames while the user thinks, and a text field showing it must keep doing so
        // without a new TextEditingEvent arriving every frame. Per SDL3's documented TextEditing
        // contract (empty composition text == composition finished, see apply(const
        // TextEditingEvent&)'s own comment), `composing()` is always exactly `!composition_text().
        // empty()` — kept as its own named query only for readability at call sites that just want
        // to know "is an IME mid-edit right now," e.g. to swallow Enter/Escape instead of treating
        // them as widget commands.
        [[nodiscard]] std::string_view composition_text() const noexcept { return composition_text_; }
        [[nodiscard]] bool composing() const noexcept { return composing_; }

        [[nodiscard]] bool mouse_down(Platform::Windowing::MouseButton button) const noexcept {
            return get(mouse_pressed_, button);
        }
        [[nodiscard]] bool mouse_just_pressed(Platform::Windowing::MouseButton button) const noexcept {
            return get(mouse_just_pressed_, button);
        }
        [[nodiscard]] bool mouse_just_released(Platform::Windowing::MouseButton button) const noexcept {
            return get(mouse_just_released_, button);
        }

        [[nodiscard]] f32 mouse_x() const noexcept { return mouse_x_; }
        [[nodiscard]] f32 mouse_y() const noexcept { return mouse_y_; }
        // Raw/unaccelerated once Window::set_relative_mouse_mode(true) is engaged (SDL3's relative
        // mode is unscaled by default; GLFW additionally gets GLFW_RAW_MOUSE_MOTION — see
        // SDL3Impl.cpp/GLFWImpl.cpp) — this is the low-latency path for a game's look/aim input.
        [[nodiscard]] f32 mouse_delta_x() const noexcept { return mouse_delta_x_; }
        [[nodiscard]] f32 mouse_delta_y() const noexcept { return mouse_delta_y_; }
        [[nodiscard]] f32 wheel_delta_x() const noexcept { return wheel_delta_x_; }
        [[nodiscard]] f32 wheel_delta_y() const noexcept { return wheel_delta_y_; }

      private:
        [[nodiscard]] static bool get(const std::vector<bool> &bits, KeyboardKey key) noexcept {
            const usize index = static_cast<usize>(key);
            return index < bits.size() && bits[index];
        }
        [[nodiscard]] static bool get(const std::vector<bool> &bits, Platform::Windowing::MouseButton button) noexcept {
            const usize index = static_cast<usize>(button);
            return index < bits.size() && bits[index];
        }

        // Sized to KeyboardKey's highest enumerator (MediaStop, in the 0x400 block) + 1 — a
        // vector<bool> indexed directly by key value, same explicit-storage-mechanism convention
        // Foundation::Cpu::Extensions already uses for its own per-flag state, chosen over
        // unordered_set<KeyboardKey> for O(1) lookup with no hashing on this per-tick-hot path.
        static constexpr usize key_count = 0x407;
        std::vector<bool> key_pressed_ = std::vector<bool>(key_count, false);
        std::vector<bool> key_just_pressed_ = std::vector<bool>(key_count, false);
        std::vector<bool> key_just_released_ = std::vector<bool>(key_count, false);
        std::string text_this_tick_;
        std::string composition_text_;
        bool composing_ = false;

        static constexpr usize mouse_button_count = 16; // MouseButton's dense range fits comfortably
        std::vector<bool> mouse_pressed_ = std::vector<bool>(mouse_button_count, false);
        std::vector<bool> mouse_just_pressed_ = std::vector<bool>(mouse_button_count, false);
        std::vector<bool> mouse_just_released_ = std::vector<bool>(mouse_button_count, false);

        f32 mouse_x_ = 0.0f;
        f32 mouse_y_ = 0.0f;
        f32 mouse_delta_x_ = 0.0f;
        f32 mouse_delta_y_ = 0.0f;
        f32 wheel_delta_x_ = 0.0f;
        f32 wheel_delta_y_ = 0.0f;
    };

} // namespace SFT::Engine

SFT_ECS_RESOURCE(SFT::Engine::InputState, "sturdy.engine.input_state");
