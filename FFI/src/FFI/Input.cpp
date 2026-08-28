/// C ABI implementation of the engine input queries.
///
/// These read the engine's `InputState` for the current tick. Out-of-range key codes deliberately
/// report "not held" rather than failing: a binding translating from another platform's key
/// enumeration will inevitably produce codes this engine has no concept of, and a hard error there
/// would make the common case (polling a set of keys every frame) fragile for no safety gain.
/// `InputState`'s own accessors already bounds-check, so nothing is dereferenced out of range.

#include <Foundation/Foundation.hpp>

#include <utility>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;
    using SFT::f32;
    using SFT::i32;

    /// Translates an ABI mouse button to the engine's own enumeration.
    ///
    /// @param button Value received from the caller.
    /// @param out_button Receives the translated value.
    ///
    /// @return Returns `true` when `button` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_mouse_button(SturdyMouseButton button,
                                              SFT::WindowManager::MouseButton *out_button) noexcept {
        switch (button) {
        case STURDY_MOUSE_BUTTON_UNKNOWN:
            *out_button = SFT::WindowManager::MouseButton::Unknown;
            return true;
        case STURDY_MOUSE_BUTTON_LEFT:
            *out_button = SFT::WindowManager::MouseButton::Left;
            return true;
        case STURDY_MOUSE_BUTTON_MIDDLE:
            *out_button = SFT::WindowManager::MouseButton::Middle;
            return true;
        case STURDY_MOUSE_BUTTON_RIGHT:
            *out_button = SFT::WindowManager::MouseButton::Right;
            return true;
        case STURDY_MOUSE_BUTTON_EXTRA1:
            *out_button = SFT::WindowManager::MouseButton::Extra1;
            return true;
        case STURDY_MOUSE_BUTTON_EXTRA2:
            *out_button = SFT::WindowManager::MouseButton::Extra2;
            return true;
        case STURDY_MOUSE_BUTTON_FORCE_U32:
        default:
            return false;
        }
    }

    /// Shared body for the three key-state queries, which differ only in which `InputState`
    /// accessor they call.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param key Engine key code, possibly out of range.
    /// @param out_value Destination flag; must not be null.
    /// @param query Accessor invoked with the resolved input state and translated key.
    ///
    /// @return `STURDY_OK`, or the handle/argument failure encountered.
    /// @note This function does not throw exceptions.
    template <typename Query>
    [[nodiscard]] SturdyResult query_key(SturdyEngine engine,
                                         i32 key,
                                         SturdyBool *out_value,
                                         Query query) noexcept {
        if (out_value == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto key_code = static_cast<SFT::Engine::KeyboardKey>(key);
        *out_value = query(resolved_engine->input_state(), key_code) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    }

    /// Shared body for the paired-float readers (position, mouse delta, wheel delta).
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_x Destination for the horizontal component, or null to skip it.
    /// @param out_y Destination for the vertical component, or null to skip it.
    /// @param read Accessor invoked with the resolved input state, returning the pair.
    ///
    /// @return `STURDY_OK`, or the handle failure encountered.
    /// @note This function does not throw exceptions.
    template <typename Read>
    [[nodiscard]] SturdyResult query_axis_pair(SturdyEngine engine,
                                               float *out_x,
                                               float *out_y,
                                               Read read) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto [x, y] = read(resolved_engine->input_state());
        if (out_x != nullptr) {
            *out_x = x;
        }
        if (out_y != nullptr) {
            *out_y = y;
        }
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_engine_key_down(SturdyEngine engine,
                                                    int32_t key,
                                                    SturdyBool *out_down) {
    return guarded([&]() -> SturdyResult {
        return query_key(engine, key, out_down,
                         [](const SFT::Engine::InputState &input, SFT::Engine::KeyboardKey code) {
                             return input.key_down(code);
                         });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_key_just_pressed(SturdyEngine engine,
                                                            int32_t key,
                                                            SturdyBool *out_pressed) {
    return guarded([&]() -> SturdyResult {
        return query_key(engine, key, out_pressed,
                         [](const SFT::Engine::InputState &input, SFT::Engine::KeyboardKey code) {
                             return input.key_just_pressed(code);
                         });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_key_just_released(SturdyEngine engine,
                                                             int32_t key,
                                                             SturdyBool *out_released) {
    return guarded([&]() -> SturdyResult {
        return query_key(engine, key, out_released,
                         [](const SFT::Engine::InputState &input, SFT::Engine::KeyboardKey code) {
                             return input.key_just_released(code);
                         });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_mouse_down(SturdyEngine engine,
                                                      SturdyMouseButton button,
                                                      SturdyBool *out_down) {
    return guarded([&]() -> SturdyResult {
        if (out_down == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::WindowManager::MouseButton engine_button{};
        if (!translate_mouse_button(button, &engine_button)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized mouse button");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        *out_down = resolved_engine->input_state().mouse_down(engine_button) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_mouse_position(SturdyEngine engine,
                                                          float *out_x,
                                                          float *out_y) {
    return guarded([&]() -> SturdyResult {
        return query_axis_pair(engine, out_x, out_y, [](const SFT::Engine::InputState &input) {
            return std::pair<f32, f32>{input.mouse_x(), input.mouse_y()};
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_mouse_delta(SturdyEngine engine,
                                                       float *out_x,
                                                       float *out_y) {
    return guarded([&]() -> SturdyResult {
        return query_axis_pair(engine, out_x, out_y, [](const SFT::Engine::InputState &input) {
            return std::pair<f32, f32>{input.mouse_delta_x(), input.mouse_delta_y()};
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_engine_wheel_delta(SturdyEngine engine,
                                                       float *out_x,
                                                       float *out_y) {
    return guarded([&]() -> SturdyResult {
        return query_axis_pair(engine, out_x, out_y, [](const SFT::Engine::InputState &input) {
            return std::pair<f32, f32>{input.wheel_delta_x(), input.wheel_delta_y()};
        });
    });
}

} // extern "C"
