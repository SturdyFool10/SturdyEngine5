use sturdy_sys::{ffi, EngineView};

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
}
