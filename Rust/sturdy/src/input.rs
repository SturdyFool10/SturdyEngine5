use sturdy_sys::window::ffi as window_ffi;
use sturdy_sys::{ffi, EngineView};

use crate::gamepad::{GamepadEvent, GamepadState};
use crate::keys::Key;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum MouseButton {
    Left = 1,
    Middle = 2,
    Right = 3,
    Extra1 = 4,
    Extra2 = 5,
    Extra3 = 6,
    Extra4 = 7,
}

/// Held keyboard modifiers (Shift/Ctrl/Alt/Super plus the CapsLock/NumLock toggles).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Modifiers {
    pub shift: bool,
    pub control: bool,
    pub alt: bool,
    /// The Super/Windows/Command key.
    pub super_key: bool,
    pub caps_lock: bool,
    pub num_lock: bool,
}

impl Modifiers {
    fn from_bits(bits: u32) -> Self {
        Self {
            shift: bits & 1 != 0,
            control: bits & 2 != 0,
            alt: bits & 4 != 0,
            super_key: bits & 8 != 0,
            caps_lock: bits & 16 != 0,
            num_lock: bits & 32 != 0,
        }
    }
}

/// Read-only view of this tick's keyboard and mouse state.
#[derive(Clone, Copy)]
pub struct Input<'a> {
    view: &'a EngineView,
}

impl<'a> Input<'a> {
    pub(crate) fn new(view: &'a EngineView) -> Self {
        Self { view }
    }

    pub fn key_down(&self, key: Key) -> bool {
        ffi::input_key_down(self.view, key.code())
    }
    pub fn key_just_pressed(&self, key: Key) -> bool {
        ffi::input_key_just_pressed(self.view, key.code())
    }
    pub fn key_just_released(&self, key: Key) -> bool {
        ffi::input_key_just_released(self.view, key.code())
    }

    pub fn mouse_down(&self, button: MouseButton) -> bool {
        ffi::input_mouse_down(self.view, button as u8)
    }
    pub fn mouse_just_pressed(&self, button: MouseButton) -> bool {
        ffi::input_mouse_just_pressed(self.view, button as u8)
    }
    pub fn mouse_just_released(&self, button: MouseButton) -> bool {
        ffi::input_mouse_just_released(self.view, button as u8)
    }

    /// Cursor position in window pixels.
    pub fn mouse_position(&self) -> glam::Vec2 {
        ffi::input_mouse_position(self.view).into()
    }
    pub fn mouse_delta(&self) -> glam::Vec2 {
        ffi::input_mouse_delta(self.view).into()
    }
    pub fn wheel_delta(&self) -> glam::Vec2 {
        ffi::input_wheel_delta(self.view).into()
    }

    /// The keyboard modifiers held this tick.
    pub fn modifiers(&self) -> Modifiers {
        Modifiers::from_bits(ffi::input_modifiers(self.view))
    }

    /// Text typed this tick (after IME composition), as UTF-8.
    pub fn text(&self) -> String {
        ffi::input_text_this_tick(self.view)
    }

    /// Every finger on a touch screen or pad this tick. A lifted finger is listed for the tick it lifted in, then
    /// disappears. Needs a window provider that delivers touch ([`WindowFeature::TouchInput`](crate::window::WindowFeature)).
    pub fn touches(&self) -> Vec<TouchPoint> {
        window_ffi::input_touches(self.view)
            .into_iter()
            .map(|t| TouchPoint {
                window_id: t.window,
                device: t.device,
                finger: t.finger,
                position: glam::Vec2::new(t.x, t.y),
                delta: glam::Vec2::new(t.dx, t.dy),
                pressure: t.pressure,
                down: t.down,
                just_pressed: t.just_pressed,
                just_released: t.just_released,
                cancelled: t.cancelled,
            })
            .collect()
    }

    /// Every pen in range this tick (and the ones that left it this tick). Needs a window provider that delivers pens
    /// ([`WindowFeature::PenInput`](crate::window::WindowFeature)).
    pub fn pens(&self) -> Vec<PenPoint> {
        window_ffi::input_pens(self.view)
            .into_iter()
            .map(|p| PenPoint {
                window_id: p.window,
                pen: p.pen,
                position: glam::Vec2::new(p.x, p.y),
                in_range: p.in_range,
                down: p.down,
                eraser: p.eraser,
                just_down: p.just_down,
                just_up: p.just_up,
                pressure: p.pressure,
                tilt: glam::Vec2::new(p.x_tilt, p.y_tilt),
                distance: p.distance,
                rotation: p.rotation,
                slider: p.slider,
                tangential_pressure: p.tangential_pressure,
                buttons: p.buttons,
            })
            .collect()
    }

    /// Every mapped gamepad as it is this tick. Empty until [`Engine::set_gamepads_enabled`] is turned on.
    pub fn gamepads(&self) -> Vec<GamepadFrame> {
        window_ffi::input_gamepads(self.view)
            .into_iter()
            .map(|f| GamepadFrame {
                id: f.id,
                state: GamepadState { buttons: f.buttons, axes: f.axes },
                pressed_this_tick: f.pressed_this_tick,
                released_this_tick: f.released_this_tick,
            })
            .collect()
    }

    /// The frame of gamepad `id`, or `None` when it is not connected (or not a mapped gamepad).
    pub fn gamepad(&self, id: u32) -> Option<GamepadFrame> {
        self.gamepads().into_iter().find(|frame| frame.id == id)
    }

    /// Gamepads plugged or unplugged since the previous tick.
    pub fn gamepad_events(&self) -> Vec<GamepadEvent> {
        window_ffi::input_gamepad_events(self.view)
            .into_iter()
            .map(|e| if e.kind == 0 { GamepadEvent::Connected(e.id) } else { GamepadEvent::Disconnected(e.id) })
            .collect()
    }
}

impl crate::Engine<'_> {
    /// Starts (or stops) sampling gamepads every tick. Off by default, because sampling starts the joystick subsystem.
    pub fn set_gamepads_enabled(&mut self, enabled: bool) {
        window_ffi::input_set_gamepads_enabled(self.view_mut(), enabled);
    }
}

/// One finger. Positions are in window coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TouchPoint {
    pub window_id: u64,
    pub device: u64,
    pub finger: u64,
    pub position: glam::Vec2,
    /// Movement since the previous tick.
    pub delta: glam::Vec2,
    pub pressure: f32,
    pub down: bool,
    pub just_pressed: bool,
    pub just_released: bool,
    /// The system took the touch away (a palm, a gesture): treat it as released without activating anything.
    pub cancelled: bool,
}

/// One pen. Positions are in window coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PenPoint {
    pub window_id: u64,
    pub pen: u32,
    pub position: glam::Vec2,
    pub in_range: bool,
    /// The tip is on the surface.
    pub down: bool,
    /// The eraser end is in use.
    pub eraser: bool,
    pub just_down: bool,
    pub just_up: bool,
    /// 0 to 1.
    pub pressure: f32,
    /// Tilt in degrees (x, y).
    pub tilt: glam::Vec2,
    /// Hover distance, 0 to 1.
    pub distance: f32,
    /// Barrel rotation in degrees.
    pub rotation: f32,
    pub slider: f32,
    pub tangential_pressure: f32,
    /// Bit `n` is barrel button `n + 1`.
    pub buttons: u32,
}

/// One mapped gamepad as it is this tick.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GamepadFrame {
    pub id: u32,
    pub state: GamepadState,
    /// Buttons that went down / up since the previous tick (bit `n` is [`Button`](crate::gamepad::Button) `n`).
    pub pressed_this_tick: u32,
    pub released_this_tick: u32,
}

impl GamepadFrame {
    pub fn down(&self, button: crate::gamepad::Button) -> bool {
        self.state.down(button)
    }
    pub fn just_pressed(&self, button: crate::gamepad::Button) -> bool {
        (self.pressed_this_tick >> button as u32) & 1 != 0
    }
    pub fn just_released(&self, button: crate::gamepad::Button) -> bool {
        (self.released_this_tick >> button as u32) & 1 != 0
    }
    pub fn axis(&self, axis: crate::gamepad::Axis) -> f32 {
        self.state.axis(axis)
    }
}
