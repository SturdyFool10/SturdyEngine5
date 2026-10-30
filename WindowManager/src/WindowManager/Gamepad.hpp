#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <array>
#include <expected>
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <WindowManager/WindowError.hpp>

// Gamepads and joysticks belong to the process, not to a window: they keep reporting when no window has focus, and a
// program has the same pads whichever window provider draws its windows. GLFW's joystick API is a strict subset of SDL's
// (no hot-plug mapping database growth, rumble, LEDs, sensors, touchpads or battery), so one implementation on SDL serves
// both providers.
namespace SFT::WindowManager::Gamepads {

    /// The standard gamepad layout, in SDL's order.
    enum class Button : u8 {
        South,
        East,
        West,
        North,
        Back,
        Guide,
        Start,
        LeftStick,
        RightStick,
        LeftShoulder,
        RightShoulder,
        DpadUp,
        DpadDown,
        DpadLeft,
        DpadRight,
        Misc1,
        RightPaddle1,
        LeftPaddle1,
        RightPaddle2,
        LeftPaddle2,
        Touchpad,
        Misc2,
        Misc3,
        Misc4,
        Misc5,
        Misc6,
        Count,
    };

    enum class Axis : u8 {
        LeftX,
        LeftY,
        RightX,
        RightY,
        /// 0 released to 1 fully pressed (the sticks run -1 to 1).
        LeftTrigger,
        RightTrigger,
        Count,
    };

    enum class DeviceKind : u8 {
        Unknown,
        Standard,
        Xbox360,
        XboxOne,
        Ps3,
        Ps4,
        Ps5,
        SwitchPro,
        JoyConLeft,
        JoyConRight,
        JoyConPair,
        GameCube,
        Steam,
    };

    enum class Connection : u8 {
        Unknown,
        Wired,
        Wireless,
    };

    enum class SensorKind : u8 {
        Accelerometer,
        Gyroscope,
    };

    /// One connected device. `mapped` devices follow the standard gamepad layout (`state`); others are raw joysticks
    /// (`raw_state`).
    struct Info {
        /// Stable while the device stays connected; pass it to the other functions.
        u32 id = 0;
        std::string name;
        DeviceKind kind = DeviceKind::Unknown;
        u16 vendor = 0;
        u16 product = 0;
        Connection connection = Connection::Unknown;
        /// The player slot (0-based), or -1.
        i32 player_index = -1;
        bool mapped = false;
        bool has_rumble = false;
        bool has_trigger_rumble = false;
        bool has_led = false;
        bool has_accelerometer = false;
        bool has_gyroscope = false;
        bool has_touchpad = false;
        /// -1 when unknown.
        i32 battery_percent = -1;
        bool charging = false;
    };

    struct State {
        /// Bit `n` is `Button(n)`.
        u32 buttons = 0;
        std::array<f32, static_cast<usize>(Axis::Count)> axes{};

        [[nodiscard]] constexpr bool down(Button button) const noexcept { return (buttons >> static_cast<u32>(button)) & 1u; }
        [[nodiscard]] constexpr f32 axis(Axis which) const noexcept { return axes[static_cast<usize>(which)]; }
    };

    /// A device as the hardware reports it, for joysticks with no gamepad mapping (flight sticks, wheels, pedals).
    struct RawState {
        /// -1 to 1.
        std::vector<f32> axes;
        std::vector<u8> buttons;
        /// Bit set per hat: 1 up, 2 right, 4 down, 8 left.
        std::vector<u8> hats;
    };

    enum class EventKind : u8 {
        Connected,
        Disconnected,
    };

    struct Event {
        EventKind kind = EventKind::Connected;
        u32 id = 0;
    };

    /// Every connected device (also starts the joystick subsystem on first use).
    [[nodiscard]] std::expected<std::vector<Info>, WindowError> list() noexcept;
    /// The current state of a mapped gamepad; empty for an unknown or unmapped device.
    [[nodiscard]] std::optional<State> state(u32 id) noexcept;
    [[nodiscard]] std::optional<RawState> raw_state(u32 id) noexcept;
    /// Devices connected or disconnected since the last call.
    [[nodiscard]] std::vector<Event> take_events() noexcept;

    /// Motor strengths are 0..1; the rumble stops after `duration_ms`.
    [[nodiscard]] std::expected<void, WindowError> rumble(u32 id, f32 low_frequency, f32 high_frequency, u32 duration_ms) noexcept;
    /// The trigger motors some pads have (Xbox One).
    [[nodiscard]] std::expected<void, WindowError> rumble_triggers(u32 id, f32 left, f32 right, u32 duration_ms) noexcept;
    [[nodiscard]] std::expected<void, WindowError> set_led(u32 id, u8 red, u8 green, u8 blue) noexcept;
    [[nodiscard]] std::expected<void, WindowError> set_player_index(u32 id, i32 player_index) noexcept;
    [[nodiscard]] std::expected<void, WindowError> set_sensor_enabled(u32 id, SensorKind sensor, bool enabled) noexcept;
    /// x, y, z of an enabled sensor (m/s^2 for the accelerometer, rad/s for the gyroscope).
    [[nodiscard]] std::expected<std::array<f32, 3>, WindowError> sensor(u32 id, SensorKind sensor) noexcept;

} // namespace SFT::WindowManager::Gamepads
