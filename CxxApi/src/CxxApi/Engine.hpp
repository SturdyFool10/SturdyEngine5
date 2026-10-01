#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <Engine/Engine.hpp>

#include <CxxApi/Types/Runtime.hpp>

namespace SFT::CxxApi {

    // ---- time ----

    /// Scaled seconds since the previous update tick.
    [[nodiscard]] double engine_delta_seconds(const Engine::Engine &engine) noexcept;
    [[nodiscard]] double engine_unscaled_delta_seconds(const Engine::Engine &engine) noexcept;
    [[nodiscard]] std::uint64_t engine_tick_index(const Engine::Engine &engine) noexcept;
    [[nodiscard]] double engine_time_scale(const Engine::Engine &engine) noexcept;
    void engine_set_time_scale(Engine::Engine &engine, double scale) noexcept;

    // ---- device ----

    /// Fills `out` and returns true when a graphics device is active.
    [[nodiscard]] bool engine_gpu_description(const Engine::Engine &engine, GpuDescription &out);
    /// One of the active GPU's descriptive strings; empty when no device is active.
    [[nodiscard]] std::unique_ptr<std::string> engine_gpu_string(const Engine::Engine &engine, GpuString which);
    [[nodiscard]] RendererCapabilities engine_capabilities(const Engine::Engine &engine) noexcept;
    /// Blocks until the GPU has finished all submitted work.
    void engine_wait_idle(Engine::Engine &engine) noexcept;

    // ---- input (state for the current update tick) ----

    /// `key` is a `WindowManager::KeyboardKey` value (the Rust `Key` enum mirrors them).
    [[nodiscard]] bool input_key_down(const Engine::Engine &engine, std::int32_t key) noexcept;
    [[nodiscard]] bool input_key_just_pressed(const Engine::Engine &engine, std::int32_t key) noexcept;
    [[nodiscard]] bool input_key_just_released(const Engine::Engine &engine, std::int32_t key) noexcept;
    /// `button` is a `WindowManager::MouseButton` value.
    [[nodiscard]] bool input_mouse_down(const Engine::Engine &engine, std::uint8_t button) noexcept;
    [[nodiscard]] bool input_mouse_just_pressed(const Engine::Engine &engine, std::uint8_t button) noexcept;
    [[nodiscard]] bool input_mouse_just_released(const Engine::Engine &engine, std::uint8_t button) noexcept;
    /// Window coordinates; see `window_pixel_density` for framebuffer pixels.
    [[nodiscard]] std::array<float, 2> input_mouse_position(const Engine::Engine &engine) noexcept;
    [[nodiscard]] std::array<float, 2> input_mouse_delta(const Engine::Engine &engine) noexcept;
    [[nodiscard]] std::array<float, 2> input_wheel_delta(const Engine::Engine &engine) noexcept;
    [[nodiscard]] std::unique_ptr<std::string> input_text_this_tick(const Engine::Engine &engine);
    /// In-progress IME composition text; empty when not composing.
    [[nodiscard]] std::unique_ptr<std::string> input_composition_text(const Engine::Engine &engine);
    [[nodiscard]] bool input_composing(const Engine::Engine &engine) noexcept;
    /// `KeyModifiers` bitmask: Shift=1, Control=2, Alt=4, Super=8, CapsLock=16, NumLock=32.
    [[nodiscard]] std::uint32_t input_modifiers(const Engine::Engine &engine) noexcept;

    // ---- input injection (same path as platform events; for tests, replays, remote input) ----

    void input_inject_key(Engine::Engine &engine, std::uint64_t window, std::int32_t key, bool pressed, std::uint32_t modifiers, bool repeat);
    void input_inject_text(Engine::Engine &engine, std::uint64_t window, const std::string &utf8);
    void input_inject_mouse_move(Engine::Engine &engine, std::uint64_t window, std::array<float, 2> position, std::array<float, 2> delta,
                                 std::uint32_t buttons);
    void input_inject_mouse_button(Engine::Engine &engine, std::uint64_t window, std::uint8_t button, bool pressed,
                                   std::array<float, 2> position, std::uint8_t clicks);
    void input_inject_mouse_wheel(Engine::Engine &engine, std::uint64_t window, std::array<float, 2> delta, std::array<float, 2> position);

} // namespace SFT::CxxApi
