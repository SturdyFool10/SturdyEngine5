#include <WindowManager/Gamepad.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>

using std::expected;
using std::unexpected;

namespace SFT::WindowManager::Gamepads {

    namespace {

        static_assert(static_cast<int>(Button::Count) == SDL_GAMEPAD_BUTTON_COUNT, "the button list must mirror SDL's");
        static_assert(static_cast<int>(Axis::Count) == SDL_GAMEPAD_AXIS_COUNT, "the axis list must mirror SDL's");

        std::mutex g_mutex;
        std::map<u32, SDL_Gamepad *> g_gamepads;
        std::map<u32, SDL_Joystick *> g_joysticks;
        std::set<u32> g_known;
        std::vector<Event> g_events;
        bool g_started = false;

        [[nodiscard]] WindowError failure(WindowErrorCode code, const char *fallback) noexcept {
            const char *message = SDL_GetError();
            return WindowError{code, message != nullptr && message[0] != '\0' ? message : fallback};
        }

        [[nodiscard]] bool ensure_started() noexcept {
            if (g_started) {
                return true;
            }
            // A game keeps reading its pads while another window has focus.
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
            if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
                return false;
            }
            g_started = true;
            return true;
        }

        /// Refreshes the devices, opens new ones, closes vanished ones, and queues the connect/disconnect events. The
        /// lock is held by the caller.
        void refresh_locked() noexcept {
            SDL_UpdateJoysticks();
            SDL_UpdateGamepads();
            std::set<u32> present;
            int count = 0;
            if (SDL_JoystickID *ids = SDL_GetJoysticks(&count)) {
                for (int i = 0; i < count; ++i) {
                    present.insert(ids[i]);
                }
                SDL_free(ids);
            }
            for (const u32 id : present) {
                if (g_known.insert(id).second) {
                    g_events.push_back(Event{EventKind::Connected, id});
                }
                if (SDL_IsGamepad(id)) {
                    if (!g_gamepads.contains(id)) {
                        if (SDL_Gamepad *pad = SDL_OpenGamepad(id)) {
                            g_gamepads.emplace(id, pad);
                        }
                    }
                } else if (!g_joysticks.contains(id)) {
                    if (SDL_Joystick *stick = SDL_OpenJoystick(id)) {
                        g_joysticks.emplace(id, stick);
                    }
                }
            }
            for (auto it = g_known.begin(); it != g_known.end();) {
                if (present.contains(*it)) {
                    ++it;
                    continue;
                }
                g_events.push_back(Event{EventKind::Disconnected, *it});
                if (const auto pad = g_gamepads.find(*it); pad != g_gamepads.end()) {
                    SDL_CloseGamepad(pad->second);
                    g_gamepads.erase(pad);
                }
                if (const auto stick = g_joysticks.find(*it); stick != g_joysticks.end()) {
                    SDL_CloseJoystick(stick->second);
                    g_joysticks.erase(stick);
                }
                it = g_known.erase(it);
            }
        }

        [[nodiscard]] DeviceKind kind_of(SDL_GamepadType type) noexcept {
            switch (type) {
                case SDL_GAMEPAD_TYPE_STANDARD: return DeviceKind::Standard;
                case SDL_GAMEPAD_TYPE_XBOX360: return DeviceKind::Xbox360;
                case SDL_GAMEPAD_TYPE_XBOXONE: return DeviceKind::XboxOne;
                case SDL_GAMEPAD_TYPE_PS3: return DeviceKind::Ps3;
                case SDL_GAMEPAD_TYPE_PS4: return DeviceKind::Ps4;
                case SDL_GAMEPAD_TYPE_PS5: return DeviceKind::Ps5;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return DeviceKind::SwitchPro;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return DeviceKind::JoyConLeft;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return DeviceKind::JoyConRight;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return DeviceKind::JoyConPair;
                case SDL_GAMEPAD_TYPE_GAMECUBE: return DeviceKind::GameCube;
                // SDL_GAMEPAD_TYPE_STEAM exists only in newer SDL3 than the 3.4.10 this project pins; its value is 12.
                case static_cast<SDL_GamepadType>(12): return DeviceKind::Steam;
                default: return DeviceKind::Unknown;
            }
        }

        [[nodiscard]] Connection connection_of(SDL_JoystickConnectionState state) noexcept {
            return state == SDL_JOYSTICK_CONNECTION_WIRED      ? Connection::Wired
                   : state == SDL_JOYSTICK_CONNECTION_WIRELESS ? Connection::Wireless
                                                               : Connection::Unknown;
        }

        [[nodiscard]] SDL_SensorType sensor_type(SensorKind kind) noexcept {
            return kind == SensorKind::Accelerometer ? SDL_SENSOR_ACCEL : SDL_SENSOR_GYRO;
        }

        [[nodiscard]] u16 strength(f32 value) noexcept {
            return static_cast<u16>(std::clamp(value, 0.0f, 1.0f) * 65535.0f + 0.5f);
        }

        [[nodiscard]] expected<SDL_Gamepad *, WindowError> pad_for(u32 id) noexcept {
            if (!ensure_started()) {
                return unexpected(failure(WindowErrorCode::BackendUnavailable, "SDL3 could not start its gamepad subsystem."));
            }
            refresh_locked();
            const auto found = g_gamepads.find(id);
            if (found == g_gamepads.end()) {
                return unexpected(WindowError{WindowErrorCode::InvalidArgument, "No such gamepad is connected."});
            }
            return found->second;
        }

        [[nodiscard]] expected<void, WindowError> result_of(bool ok, const char *fallback) noexcept {
            return ok ? expected<void, WindowError>{} : unexpected(failure(WindowErrorCode::Unsupported, fallback));
        }

    } // namespace

    expected<std::vector<Info>, WindowError> list() noexcept {
        const std::lock_guard lock{g_mutex};
        if (!ensure_started()) {
            return unexpected(failure(WindowErrorCode::BackendUnavailable, "SDL3 could not start its gamepad subsystem."));
        }
        refresh_locked();
        std::vector<Info> out;
        try {
            for (const u32 id : g_known) {
                Info info;
                info.id = id;
                const auto pad = g_gamepads.find(id);
                if (pad != g_gamepads.end()) {
                    SDL_Gamepad *g = pad->second;
                    info.mapped = true;
                    if (const char *name = SDL_GetGamepadName(g)) {
                        info.name = name;
                    }
                    info.kind = kind_of(SDL_GetGamepadType(g));
                    info.vendor = SDL_GetGamepadVendor(g);
                    info.product = SDL_GetGamepadProduct(g);
                    info.connection = connection_of(SDL_GetGamepadConnectionState(g));
                    info.player_index = SDL_GetGamepadPlayerIndex(g);
                    info.has_accelerometer = SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL);
                    info.has_gyroscope = SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO);
                    info.has_touchpad = SDL_GetNumGamepadTouchpads(g) > 0;
                    const SDL_PropertiesID properties = SDL_GetGamepadProperties(g);
                    info.has_rumble = SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
                    info.has_trigger_rumble = SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);
                    info.has_led = SDL_GetBooleanProperty(properties, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
                    int percent = -1;
                    const SDL_PowerState power = SDL_GetGamepadPowerInfo(g, &percent);
                    info.battery_percent = power == SDL_POWERSTATE_ON_BATTERY || power == SDL_POWERSTATE_CHARGING ? percent : -1;
                    info.charging = power == SDL_POWERSTATE_CHARGING;
                } else if (const auto stick = g_joysticks.find(id); stick != g_joysticks.end()) {
                    SDL_Joystick *j = stick->second;
                    if (const char *name = SDL_GetJoystickName(j)) {
                        info.name = name;
                    }
                    info.vendor = SDL_GetJoystickVendor(j);
                    info.product = SDL_GetJoystickProduct(j);
                    info.connection = connection_of(SDL_GetJoystickConnectionState(j));
                    info.player_index = SDL_GetJoystickPlayerIndex(j);
                    const SDL_PropertiesID properties = SDL_GetJoystickProperties(j);
                    info.has_rumble = SDL_GetBooleanProperty(properties, SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
                    info.has_led = SDL_GetBooleanProperty(properties, SDL_PROP_JOYSTICK_CAP_RGB_LED_BOOLEAN, false);
                }
                out.push_back(std::move(info));
            }
        } catch (...) {
            return unexpected(WindowError{WindowErrorCode::OutOfMemory, "Out of memory listing gamepads."});
        }
        return out;
    }

    std::optional<State> state(u32 id) noexcept {
        const std::lock_guard lock{g_mutex};
        const auto pad = pad_for(id);
        if (!pad) {
            return std::nullopt;
        }
        State out;
        for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
            if (SDL_GetGamepadButton(*pad, static_cast<SDL_GamepadButton>(button))) {
                out.buttons |= 1u << button;
            }
        }
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
            const Sint16 raw = SDL_GetGamepadAxis(*pad, static_cast<SDL_GamepadAxis>(axis));
            out.axes[static_cast<usize>(axis)] = raw < 0 ? static_cast<f32>(raw) / 32768.0f : static_cast<f32>(raw) / 32767.0f;
        }
        return out;
    }

    std::optional<RawState> raw_state(u32 id) noexcept {
        const std::lock_guard lock{g_mutex};
        if (!ensure_started()) {
            return std::nullopt;
        }
        refresh_locked();
        SDL_Joystick *stick = nullptr;
        if (const auto found = g_joysticks.find(id); found != g_joysticks.end()) {
            stick = found->second;
        } else if (const auto pad = g_gamepads.find(id); pad != g_gamepads.end()) {
            stick = SDL_GetGamepadJoystick(pad->second); // a mapped pad's hardware view
        }
        if (stick == nullptr) {
            return std::nullopt;
        }
        try {
            RawState out;
            for (int i = 0; i < SDL_GetNumJoystickAxes(stick); ++i) {
                const Sint16 raw = SDL_GetJoystickAxis(stick, i);
                out.axes.push_back(raw < 0 ? static_cast<f32>(raw) / 32768.0f : static_cast<f32>(raw) / 32767.0f);
            }
            for (int i = 0; i < SDL_GetNumJoystickButtons(stick); ++i) {
                out.buttons.push_back(SDL_GetJoystickButton(stick, i) ? 1 : 0);
            }
            for (int i = 0; i < SDL_GetNumJoystickHats(stick); ++i) {
                out.hats.push_back(SDL_GetJoystickHat(stick, i));
            }
            return out;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::vector<Event> take_events() noexcept {
        const std::lock_guard lock{g_mutex};
        if (ensure_started()) {
            refresh_locked();
        }
        std::vector<Event> out;
        out.swap(g_events);
        return out;
    }

    expected<void, WindowError> rumble(u32 id, f32 low_frequency, f32 high_frequency, u32 duration_ms) noexcept {
        const std::lock_guard lock{g_mutex};
        const auto pad = pad_for(id);
        if (!pad) {
            return unexpected(pad.error());
        }
        return result_of(SDL_RumbleGamepad(*pad, strength(low_frequency), strength(high_frequency), duration_ms), "This gamepad cannot rumble.");
    }

    expected<void, WindowError> rumble_triggers(u32 id, f32 left, f32 right, u32 duration_ms) noexcept {
        const std::lock_guard lock{g_mutex};
        const auto pad = pad_for(id);
        if (!pad) {
            return unexpected(pad.error());
        }
        return result_of(SDL_RumbleGamepadTriggers(*pad, strength(left), strength(right), duration_ms), "This gamepad has no trigger motors.");
    }

    expected<void, WindowError> set_led(u32 id, u8 red, u8 green, u8 blue) noexcept {
        const std::lock_guard lock{g_mutex};
        const auto pad = pad_for(id);
        if (!pad) {
            return unexpected(pad.error());
        }
        return result_of(SDL_SetGamepadLED(*pad, red, green, blue), "This gamepad has no light.");
    }

    expected<void, WindowError> set_player_index(u32 id, i32 player_index) noexcept {
        const std::lock_guard lock{g_mutex};
        const auto pad = pad_for(id);
        if (!pad) {
            return unexpected(pad.error());
        }
        return result_of(SDL_SetGamepadPlayerIndex(*pad, player_index), "This gamepad has no player indicator.");
    }

    expected<void, WindowError> set_sensor_enabled(u32 id, SensorKind sensor, bool enabled) noexcept {
        const std::lock_guard lock{g_mutex};
        const auto pad = pad_for(id);
        if (!pad) {
            return unexpected(pad.error());
        }
        return result_of(SDL_SetGamepadSensorEnabled(*pad, sensor_type(sensor), enabled), "This gamepad has no such sensor.");
    }

    expected<std::array<f32, 3>, WindowError> sensor(u32 id, SensorKind kind) noexcept {
        const std::lock_guard lock{g_mutex};
        const auto pad = pad_for(id);
        if (!pad) {
            return unexpected(pad.error());
        }
        std::array<f32, 3> data{};
        if (!SDL_GetGamepadSensorData(*pad, sensor_type(kind), data.data(), 3)) {
            return unexpected(failure(WindowErrorCode::Unsupported, "This gamepad has no such sensor, or it is not enabled."));
        }
        return data;
    }

} // namespace SFT::WindowManager::Gamepads
