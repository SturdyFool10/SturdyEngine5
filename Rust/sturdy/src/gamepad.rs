//! Gamepads and joysticks. They belong to the process, not to a window: they keep reporting while no window has focus,
//! and a program has the same pads whichever window provider (SDL3 or GLFW) draws its windows.
//!
//! A pad with a gamepad mapping ([`GamepadInfo::mapped`]) follows the standard layout read by [`state`]; any other
//! joystick (flight stick, wheel, pedals) is read raw with [`raw_state`]. Poll once per frame; [`take_events`] reports
//! pads being plugged and unplugged.

use sturdy_sys::window::ffi;

use crate::Error;

/// The standard gamepad layout's buttons; bit `n` of [`GamepadState::buttons`] is the button with value `n`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Button {
    South = 0,
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
}

/// The standard layout's axes: the sticks run -1 to 1, the triggers 0 to 1.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Axis {
    LeftX = 0,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DeviceKind {
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
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Connection {
    Unknown,
    Wired,
    Wireless,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Sensor {
    Accelerometer = 0,
    Gyroscope = 1,
}

/// One connected device.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GamepadInfo {
    /// Stable while the device stays connected; pass it to the other functions.
    pub id: u32,
    pub name: String,
    pub kind: DeviceKind,
    pub vendor: u16,
    pub product: u16,
    pub connection: Connection,
    /// The player slot (0-based).
    pub player_index: Option<u32>,
    /// Follows the standard layout ([`state`]); false for a raw joystick ([`raw_state`]).
    pub mapped: bool,
    pub has_rumble: bool,
    pub has_trigger_rumble: bool,
    pub has_led: bool,
    pub has_accelerometer: bool,
    pub has_gyroscope: bool,
    pub has_touchpad: bool,
    /// `None` when the battery level is unknown.
    pub battery_percent: Option<u32>,
    pub charging: bool,
}

/// A mapped gamepad's current state.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GamepadState {
    /// Bit `n` is the [`Button`] with value `n`.
    pub buttons: u32,
    pub axes: [f32; 6],
}

impl GamepadState {
    pub fn down(&self, button: Button) -> bool {
        (self.buttons >> button as u32) & 1 != 0
    }

    pub fn axis(&self, axis: Axis) -> f32 {
        self.axes[axis as usize]
    }
}

/// A device as the hardware reports it.
#[derive(Debug, Clone, PartialEq)]
pub struct RawState {
    /// -1 to 1.
    pub axes: Vec<f32>,
    pub buttons: Vec<bool>,
    /// A bit set per hat: 1 up, 2 right, 4 down, 8 left.
    pub hats: Vec<u8>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GamepadEvent {
    Connected(u32),
    Disconnected(u32),
}

fn status(s: ffi::WindowStatus) -> Result<(), Error> {
    if s.ok { Ok(()) } else { Err(Error::new(s.error)) }
}

/// Every connected device (also starts the joystick subsystem on first use).
pub fn list() -> Result<Vec<GamepadInfo>, Error> {
    let mut error = String::new();
    let devices = ffi::gamepad_list(&mut error);
    if !error.is_empty() {
        return Err(Error::new(error));
    }
    Ok(devices
        .into_iter()
        .map(|d| GamepadInfo {
            id: d.id,
            name: d.name,
            kind: match d.kind {
                1 => DeviceKind::Standard,
                2 => DeviceKind::Xbox360,
                3 => DeviceKind::XboxOne,
                4 => DeviceKind::Ps3,
                5 => DeviceKind::Ps4,
                6 => DeviceKind::Ps5,
                7 => DeviceKind::SwitchPro,
                8 => DeviceKind::JoyConLeft,
                9 => DeviceKind::JoyConRight,
                10 => DeviceKind::JoyConPair,
                11 => DeviceKind::GameCube,
                12 => DeviceKind::Steam,
                _ => DeviceKind::Unknown,
            },
            vendor: d.vendor,
            product: d.product,
            connection: match d.connection {
                1 => Connection::Wired,
                2 => Connection::Wireless,
                _ => Connection::Unknown,
            },
            player_index: u32::try_from(d.player_index).ok(),
            mapped: d.mapped,
            has_rumble: d.has_rumble,
            has_trigger_rumble: d.has_trigger_rumble,
            has_led: d.has_led,
            has_accelerometer: d.has_accelerometer,
            has_gyroscope: d.has_gyroscope,
            has_touchpad: d.has_touchpad,
            battery_percent: u32::try_from(d.battery_percent).ok(),
            charging: d.charging,
        })
        .collect())
}

/// The current state of a mapped gamepad; `None` for an unknown or unmapped device.
pub fn state(id: u32) -> Option<GamepadState> {
    let s = ffi::gamepad_state(id);
    s.available.then_some(GamepadState { buttons: s.buttons, axes: s.axes })
}

/// A device as the hardware reports it; `None` for an unknown device.
pub fn raw_state(id: u32) -> Option<RawState> {
    let r = ffi::gamepad_raw_state(id);
    r.available.then(|| RawState { axes: r.axes, buttons: r.buttons.into_iter().map(|b| b != 0).collect(), hats: r.hats })
}

/// Devices connected or disconnected since the last call.
pub fn take_events() -> Vec<GamepadEvent> {
    ffi::gamepad_take_events()
        .into_iter()
        .map(|e| if e.kind == 0 { GamepadEvent::Connected(e.id) } else { GamepadEvent::Disconnected(e.id) })
        .collect()
}

/// Motor strengths are 0..=1; the rumble stops after `duration_ms`.
pub fn rumble(id: u32, low_frequency: f32, high_frequency: f32, duration_ms: u32) -> Result<(), Error> {
    status(ffi::gamepad_rumble(id, low_frequency, high_frequency, duration_ms))
}

/// The trigger motors some pads have (Xbox One).
pub fn rumble_triggers(id: u32, left: f32, right: f32, duration_ms: u32) -> Result<(), Error> {
    status(ffi::gamepad_rumble_triggers(id, left, right, duration_ms))
}

pub fn set_led(id: u32, red: u8, green: u8, blue: u8) -> Result<(), Error> {
    status(ffi::gamepad_set_led(id, red, green, blue))
}

pub fn set_player_index(id: u32, player_index: i32) -> Result<(), Error> {
    status(ffi::gamepad_set_player_index(id, player_index))
}

pub fn set_sensor_enabled(id: u32, sensor: Sensor, enabled: bool) -> Result<(), Error> {
    status(ffi::gamepad_set_sensor_enabled(id, sensor as u8, enabled))
}

/// x, y, z of an enabled sensor (m/s² for the accelerometer, rad/s for the gyroscope).
pub fn sensor(id: u32, sensor: Sensor) -> Result<[f32; 3], Error> {
    let r = ffi::gamepad_sensor(id, sensor as u8);
    if r.ok { Ok(r.xyz) } else { Err(Error::new("no such sensor, or it is not enabled")) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_reads_buttons_and_axes() {
        let state = GamepadState { buttons: 1 << Button::South as u32 | 1 << Button::DpadLeft as u32, axes: [0.5, -0.5, 0.0, 0.0, 1.0, 0.0] };
        assert!(state.down(Button::South));
        assert!(state.down(Button::DpadLeft));
        assert!(!state.down(Button::East));
        assert_eq!(state.axis(Axis::LeftTrigger), 1.0);
        assert_eq!(state.axis(Axis::LeftY), -0.5);
    }

    #[test]
    fn unknown_devices_are_reported_as_such() {
        // No device has this id, with or without hardware attached.
        assert!(state(0xFFFF_FFF0).is_none());
        assert!(raw_state(0xFFFF_FFF0).is_none());
        assert!(rumble(0xFFFF_FFF0, 1.0, 1.0, 10).is_err());
    }
}
