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

    /// Ordinary World resource: accumulated per-tick input state — the read side of "is this key or
    /// mouse button currently down," which nothing in the engine could answer before this
    /// (plans/ecs-engine-subsystem-access.md's Phase 2, "InputState"). `Engine` owns the persistent
    /// instance (`EngineModule.hpp`), folded in place every tick by a built-in system
    /// (`EngineImpl.cpp`) that reads the same typed `KeyboardEvent`/`TextInputEvent`/`MouseMoveEvent`/
    /// `MouseButtonEvent`/`MouseWheelEvent` streams any other consumer already can (`EcsEvents.hpp`) —
    /// never rebound, so a `Ecs::ReadResource<InputState>` a system holds mid-tick is never invalidated
    /// by the update itself (same contract `WindowState`/`RenderFrameRequests` already rely on).
    /// `Ecs::ReadResource<InputState>` only — input flows one direction (OS -> Application -> ECS).
    ///
    /// Two distinct paths, pick deliberately:
    ///   - `key_down()`/`key_just_pressed()`/`key_just_released()` — raw scancode-driven, fires on the
    ///     physical key regardless of layout/IME, with no OS key-repeat applied (repeat is still
    ///     available directly off the raw `KeyboardEvent` stream via `.repeat`, for a system that wants
    ///     it). Lowest latency; for game actions and rebindable controls, including modifier keys —
    ///     `LeftShift`/`RightShift`/etc. are ordinary `KeyboardKey` values here, not special-cased.
    ///   - `text_this_tick()` — OS-composed UTF-8 text (`WindowTextInputEvent`), already IME/layout-
    ///     aware and already repeat-consistent with the OS's own key-repeat rate/delay for free, since
    ///     the OS is what generates the repeated `TextInput` events in the first place. For actual
    ///     typing — text fields, chat, console input.
    ///   - `modifiers()` is a coarse, derived-on-demand convenience (any-side Shift/Control/Alt/Super,
    ///     plus Caps/Num lock) for the common "is Shift held" check — it reads the same per-key state
    ///     `key_down()` does, not a second, driftable source of truth.
    class InputState {
      public:
        /// Clears just_pressed/just_released/text_this_tick/this-tick deltas — called once per tick,
        /// before this tick's events are folded in via apply().
        void begin_tick() noexcept;

        void apply(const KeyboardEvent &event) noexcept;

        void apply(const TextInputEvent &event) noexcept;

        void apply(const TextEditingEvent &event) noexcept;

        void apply(const MouseMoveEvent &event) noexcept;

        void apply(const MouseButtonEvent &event) noexcept;

        void apply(const MouseWheelEvent &event) noexcept;

        [[nodiscard]] bool key_down(KeyboardKey key) const noexcept;
        [[nodiscard]] bool key_just_pressed(KeyboardKey key) const noexcept;
        [[nodiscard]] bool key_just_released(KeyboardKey key) const noexcept;

        [[nodiscard]] Platform::Windowing::KeyModifiers modifiers() const noexcept;

        [[nodiscard]] std::string_view text_this_tick() const noexcept;

        /// Unlike text_this_tick() (a per-tick delta, cleared every begin_tick()), these describe
        /// ongoing IME state and persist across ticks where nothing changed — a composition can sit
        /// idle for many frames while the user thinks, and a text field showing it must keep doing so
        /// without a new TextEditingEvent arriving every frame. Per SDL3's documented TextEditing
        /// contract (empty composition text == composition finished, see apply(const
        /// TextEditingEvent&)'s own comment), `composing()` is always exactly `!composition_text().
        /// empty()` — kept as its own named query only for readability at call sites that just want
        /// to know "is an IME mid-edit right now," e.g. to swallow Enter/Escape instead of treating
        /// them as widget commands.
        [[nodiscard]] std::string_view composition_text() const noexcept;
        [[nodiscard]] bool composing() const noexcept;

        [[nodiscard]] bool mouse_down(Platform::Windowing::MouseButton button) const noexcept;
        [[nodiscard]] bool mouse_just_pressed(Platform::Windowing::MouseButton button) const noexcept;
        [[nodiscard]] bool mouse_just_released(Platform::Windowing::MouseButton button) const noexcept;

        [[nodiscard]] f32 mouse_x() const noexcept;
        [[nodiscard]] f32 mouse_y() const noexcept;
        /// Raw/unaccelerated once Window::set_relative_mouse_mode(true) is engaged (SDL3's relative
        /// mode is unscaled by default; GLFW additionally gets GLFW_RAW_MOUSE_MOTION — see
        /// SDL3Impl.cpp/GLFWImpl.cpp) — this is the low-latency path for a game's look/aim input.
        [[nodiscard]] f32 mouse_delta_x() const noexcept;
        [[nodiscard]] f32 mouse_delta_y() const noexcept;
        [[nodiscard]] f32 wheel_delta_x() const noexcept;
        [[nodiscard]] f32 wheel_delta_y() const noexcept;

      private:
        [[nodiscard]] static bool get(const std::vector<bool> &bits, KeyboardKey key) noexcept;
        [[nodiscard]] static bool get(const std::vector<bool> &bits, Platform::Windowing::MouseButton button) noexcept;

        /// Sized to KeyboardKey's highest enumerator (MediaStop, in the 0x400 block) + 1 — a
        /// vector<bool> indexed directly by key value, same explicit-storage-mechanism convention
        /// Foundation::Cpu::Extensions already uses for its own per-flag state, chosen over
        /// unordered_set<KeyboardKey> for O(1) lookup with no hashing on this per-tick-hot path.
        static constexpr usize key_count = 0x407;
        std::vector<bool> key_pressed_ = std::vector<bool>(key_count, false);
        std::vector<bool> key_just_pressed_ = std::vector<bool>(key_count, false);
        std::vector<bool> key_just_released_ = std::vector<bool>(key_count, false);
        std::string text_this_tick_;
        std::string composition_text_;
        bool composing_ = false;

        static constexpr usize mouse_button_count = 16;
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
