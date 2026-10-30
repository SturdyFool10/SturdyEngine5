/// C ABI implementation of the process-wide gamepad and joystick service (`WindowManager/Gamepad.hpp`).

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <Engine/Engine.hpp>
#include <WindowManager/Gamepad.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace G = SFT::WindowManager::Gamepads;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::set_error;

    [[nodiscard]] SturdyResult translate(const SFT::WindowManager::WindowError &error) noexcept {
        const bool unsupported = error.code == SFT::WindowManager::WindowErrorCode::Unsupported;
        const bool missing = error.code == SFT::WindowManager::WindowErrorCode::InvalidArgument;
        return set_error(unsupported || missing ? STURDY_ERROR_NOT_AVAILABLE : STURDY_ERROR_INTERNAL, std::string{error.message}.c_str());
    }

    [[nodiscard]] SturdyResult list_devices(std::vector<G::Info> *out) noexcept {
        auto devices = G::list();
        if (!devices) {
            return translate(devices.error());
        }
        *out = std::move(*devices);
        return STURDY_OK;
    }

    [[nodiscard]] bool valid_sensor(SturdyGamepadSensor sensor) noexcept {
        return static_cast<uint32_t>(sensor) <= static_cast<uint32_t>(STURDY_GAMEPAD_SENSOR_GYROSCOPE);
    }

    [[nodiscard]] SturdyResult from_status(const std::expected<void, SFT::WindowManager::WindowError> &status) noexcept {
        return status ? STURDY_OK : translate(status.error());
    }

    [[nodiscard]] SturdyBool flag(bool value) noexcept { return value ? STURDY_TRUE : STURDY_FALSE; }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_gamepad_count(uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        std::vector<G::Info> devices;
        if (const SturdyResult listed = list_devices(&devices); listed != STURDY_OK) {
            return listed;
        }
        *out_count = static_cast<uint32_t>(devices.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_get(uint32_t index, SturdyGamepadInfo *out_info) {
    return guarded([&]() -> SturdyResult {
        if (out_info == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        std::vector<G::Info> devices;
        if (const SturdyResult listed = list_devices(&devices); listed != STURDY_OK) {
            return listed;
        }
        if (index >= devices.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "gamepad index is out of range");
        }
        const G::Info &device = devices[index];
        *out_info = SturdyGamepadInfo{};
        out_info->struct_size = static_cast<uint32_t>(sizeof(SturdyGamepadInfo));
        out_info->id = device.id;
        out_info->kind = static_cast<SturdyGamepadKind>(device.kind);
        out_info->vendor = device.vendor;
        out_info->product = device.product;
        out_info->connection = static_cast<SturdyGamepadConnection>(device.connection);
        out_info->player_index = device.player_index;
        out_info->mapped = flag(device.mapped);
        out_info->has_rumble = flag(device.has_rumble);
        out_info->has_trigger_rumble = flag(device.has_trigger_rumble);
        out_info->has_led = flag(device.has_led);
        out_info->has_accelerometer = flag(device.has_accelerometer);
        out_info->has_gyroscope = flag(device.has_gyroscope);
        out_info->has_touchpad = flag(device.has_touchpad);
        out_info->battery_percent = device.battery_percent;
        out_info->charging = flag(device.charging);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_name(uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        std::vector<G::Info> devices;
        if (const SturdyResult listed = list_devices(&devices); listed != STURDY_OK) {
            return listed;
        }
        if (index >= devices.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "gamepad index is out of range");
        }
        return copy_string_out(devices[index].name, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_state(uint32_t id, SturdyGamepadState *out_state) {
    return guarded([&]() -> SturdyResult {
        if (out_state == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const auto state = G::state(id);
        if (!state) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such mapped gamepad is connected");
        }
        *out_state = SturdyGamepadState{};
        out_state->struct_size = static_cast<uint32_t>(sizeof(SturdyGamepadState));
        out_state->buttons = state->buttons;
        std::copy(state->axes.begin(), state->axes.end(), out_state->axes);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_raw_counts(uint32_t id, uint32_t *out_axes, uint32_t *out_buttons, uint32_t *out_hats) {
    return guarded([&]() -> SturdyResult {
        if (out_axes == nullptr || out_buttons == nullptr || out_hats == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointers must not be null");
        }
        const auto raw = G::raw_state(id);
        if (!raw) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such device is connected");
        }
        *out_axes = static_cast<uint32_t>(raw->axes.size());
        *out_buttons = static_cast<uint32_t>(raw->buttons.size());
        *out_hats = static_cast<uint32_t>(raw->hats.size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_raw_read(uint32_t id, float *axes, uint32_t axes_capacity, uint8_t *buttons, uint32_t buttons_capacity,
                                                     uint8_t *hats, uint32_t hats_capacity) {
    return guarded([&]() -> SturdyResult {
        const auto raw = G::raw_state(id);
        if (!raw) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no such device is connected");
        }
        if (axes != nullptr) {
            std::copy_n(raw->axes.begin(), std::min<size_t>(raw->axes.size(), axes_capacity), axes);
        }
        if (buttons != nullptr) {
            std::copy_n(raw->buttons.begin(), std::min<size_t>(raw->buttons.size(), buttons_capacity), buttons);
        }
        if (hats != nullptr) {
            std::copy_n(raw->hats.begin(), std::min<size_t>(raw->hats.size(), hats_capacity), hats);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_take_events(SturdyGamepadEvent *events, uint32_t capacity, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr || (events == nullptr && capacity != 0)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "an event buffer and a count pointer are required");
        }
        const std::vector<G::Event> taken = G::take_events();
        const uint32_t written = static_cast<uint32_t>(std::min<size_t>(taken.size(), capacity));
        for (uint32_t i = 0; i < written; ++i) {
            events[i] = SturdyGamepadEvent{taken[i].kind == G::EventKind::Connected ? STURDY_GAMEPAD_EVENT_CONNECTED : STURDY_GAMEPAD_EVENT_DISCONNECTED,
                                           taken[i].id};
        }
        *out_count = written;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_rumble(uint32_t id, float low_frequency, float high_frequency, uint32_t duration_ms) {
    return guarded([&]() -> SturdyResult { return from_status(G::rumble(id, low_frequency, high_frequency, duration_ms)); });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_rumble_triggers(uint32_t id, float left, float right, uint32_t duration_ms) {
    return guarded([&]() -> SturdyResult { return from_status(G::rumble_triggers(id, left, right, duration_ms)); });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_set_led(uint32_t id, uint8_t red, uint8_t green, uint8_t blue) {
    return guarded([&]() -> SturdyResult { return from_status(G::set_led(id, red, green, blue)); });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_set_player_index(uint32_t id, int32_t player_index) {
    return guarded([&]() -> SturdyResult { return from_status(G::set_player_index(id, player_index)); });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_set_sensor_enabled(uint32_t id, SturdyGamepadSensor sensor, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        if (!valid_sensor(sensor)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized gamepad sensor");
        }
        return from_status(G::set_sensor_enabled(id, static_cast<G::SensorKind>(sensor), enabled != STURDY_FALSE));
    });
}

} // extern "C"

// -- Per-tick input state (touch, pen, gamepads) ------------------------------------------------------------------------

namespace {

    /// Resolves the engine's input state and runs `fn(input)`; the readers below are all this shape.
    template <typename Fn>
    [[nodiscard]] SturdyResult with_input(SturdyEngine engine, Fn &&fn) noexcept {
        SFT::Engine::Engine *resolved = nullptr;
        const SturdyResult status = SFT::Ffi::resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        return fn(resolved->input_state());
    }

    [[nodiscard]] SturdyResult need_out(const void *pointer) noexcept {
        return pointer == nullptr ? set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null") : STURDY_OK;
    }

    [[nodiscard]] SturdyResult range_error(const char *what) noexcept { return set_error(STURDY_ERROR_OUT_OF_RANGE, what); }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_input_touch_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_count); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            *out_count = static_cast<uint32_t>(input.touches().size());
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_touch_get(SturdyEngine engine, uint32_t index, SturdyTouchPoint *out_point) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_point); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            if (index >= input.touches().size()) {
                return range_error("touch index is out of range");
            }
            const auto &touch = input.touches()[index];
            *out_point = SturdyTouchPoint{static_cast<uint64_t>(touch.window), touch.device, touch.finger, touch.x, touch.y, touch.dx, touch.dy,
                                          touch.pressure, flag(touch.down), flag(touch.just_pressed), flag(touch.just_released), flag(touch.cancelled)};
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_pen_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_count); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            *out_count = static_cast<uint32_t>(input.pens().size());
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_pen_get(SturdyEngine engine, uint32_t index, SturdyPenPoint *out_point) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_point); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            if (index >= input.pens().size()) {
                return range_error("pen index is out of range");
            }
            const auto &pen = input.pens()[index];
            *out_point = SturdyPenPoint{static_cast<uint64_t>(pen.window), pen.pen, pen.x, pen.y, flag(pen.in_range), flag(pen.down), flag(pen.eraser),
                                        flag(pen.just_down), flag(pen.just_up), pen.pressure, pen.x_tilt, pen.y_tilt, pen.distance, pen.rotation,
                                        pen.slider, pen.tangential_pressure, pen.buttons};
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_set_gamepads_enabled(SturdyEngine engine, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            input.set_gamepads_enabled(enabled != STURDY_FALSE);
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_gamepad_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_count); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            *out_count = static_cast<uint32_t>(input.gamepads().size());
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_gamepad_get(SturdyEngine engine, uint32_t index, SturdyGamepadFrame *out_frame) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_frame); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            if (index >= input.gamepads().size()) {
                return range_error("gamepad index is out of range");
            }
            const auto &frame = input.gamepads()[index];
            *out_frame = SturdyGamepadFrame{};
            out_frame->id = frame.id;
            out_frame->buttons = frame.state.buttons;
            out_frame->pressed_this_tick = frame.pressed_this_tick;
            out_frame->released_this_tick = frame.released_this_tick;
            std::copy(frame.state.axes.begin(), frame.state.axes.end(), out_frame->axes);
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_gamepad_event_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_count); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            *out_count = static_cast<uint32_t>(input.gamepad_events().size());
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_input_gamepad_event_get(SturdyEngine engine, uint32_t index, SturdyGamepadEvent *out_event) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_out(out_event); checked != STURDY_OK) {
            return checked;
        }
        return with_input(engine, [&](SFT::Engine::InputState &input) {
            if (index >= input.gamepad_events().size()) {
                return range_error("gamepad event index is out of range");
            }
            const auto &event = input.gamepad_events()[index];
            *out_event = SturdyGamepadEvent{event.kind == G::EventKind::Connected ? STURDY_GAMEPAD_EVENT_CONNECTED : STURDY_GAMEPAD_EVENT_DISCONNECTED,
                                            event.id};
            return STURDY_OK;
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_gamepad_sensor(uint32_t id, SturdyGamepadSensor sensor, float out_xyz[3]) {
    return guarded([&]() -> SturdyResult {
        if (out_xyz == nullptr || !valid_sensor(sensor)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "an output array and a known sensor are required");
        }
        const auto data = G::sensor(id, static_cast<G::SensorKind>(sensor));
        if (!data) {
            return translate(data.error());
        }
        std::copy(data->begin(), data->end(), out_xyz);
        return STURDY_OK;
    });
}

} // extern "C"
