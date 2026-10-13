//! Application-loop control for event-driven apps (tools, launchers, editors).
//!
//! By default the engine renders continuously, like a game. In [`FrameMode::OnDemand`] it renders only when
//! something asks: window input, a resize, [`Engine::request_redraw`], a due
//! [`request_redraw_after`](Engine::request_redraw_after), or a device change. Between those the main loop sleeps in
//! the window system's wait, so an idle app costs about 0% CPU and GPU, and the window manager backs its own
//! input polling off to a few hundred wakeups a second.
//!
//! ```ignore
//! fn init(&mut self, engine: &mut Engine<'_>) -> Result<(), Error> {
//!     engine.set_frame_mode(FrameMode::OnDemand);
//!     let redraw = engine.redraw_handle();
//!     std::thread::spawn(move || {
//!         // ... a download finished ...
//!         redraw.request_redraw();
//!     });
//!     Ok(())
//! }
//! ```
//!
//! Animations ask for their next frame themselves: call `request_redraw_after(Duration::from_millis(16))` (or
//! `request_redraw()`) from the frame that still has motion left.

use std::sync::Arc;
use std::time::Duration;

use sturdy_sys::app::ffi;

use crate::Engine;

/// See the [module docs](crate::app).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum FrameMode {
    #[default]
    Continuous,
    OnDemand,
}

/// Wakes the UI from any thread. Cheap to clone.
#[derive(Clone)]
pub struct RedrawHandle(Arc<cxx::UniquePtr<ffi::RedrawHandle>>);

impl std::fmt::Debug for RedrawHandle {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("RedrawHandle")
    }
}

impl RedrawHandle {
    /// Render a frame as soon as possible.
    pub fn request_redraw(&self) {
        if let Some(handle) = self.0.as_ref().as_ref() {
            ffi::redraw_request(handle);
        }
    }

    /// Render a frame within `delay` (the earliest pending request wins).
    pub fn request_redraw_after(&self, delay: Duration) {
        if let Some(handle) = self.0.as_ref().as_ref() {
            ffi::redraw_request_after(handle, u64::try_from(delay.as_nanos()).unwrap_or(u64::MAX));
        }
    }
}

impl Engine<'_> {
    /// See [`FrameMode`]. Takes effect on the next loop iteration.
    pub fn set_frame_mode(&mut self, mode: FrameMode) {
        ffi::set_frame_mode(
            self.view_mut(),
            match mode {
                FrameMode::Continuous => ffi::FrameMode::Continuous,
                FrameMode::OnDemand => ffi::FrameMode::OnDemand,
            },
        );
    }

    pub fn frame_mode(&self) -> FrameMode {
        match ffi::frame_mode(self.view()) {
            ffi::FrameMode::OnDemand => FrameMode::OnDemand,
            _ => FrameMode::Continuous,
        }
    }

    /// A handle other threads can use to wake the UI.
    pub fn redraw_handle(&self) -> RedrawHandle {
        RedrawHandle(Arc::new(ffi::redraw_handle(self.view())))
    }

    /// Render a frame as soon as possible (no-op cost in continuous mode).
    pub fn request_redraw(&self) {
        self.redraw_handle().request_redraw();
    }

    /// Render a frame within `delay`.
    pub fn request_redraw_after(&self, delay: Duration) {
        self.redraw_handle().request_redraw_after(delay);
    }
}
