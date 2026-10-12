use std::sync::atomic::{AtomicU32, Ordering};

use sturdy_plugin::{sturdy_plugin, Engine, FrameRequest, Module, ModuleOptions, Plugin};

const FRAMES_BEFORE_CLOSE: u32 = 90;

/// Supplies the engine's default scene and asks the window to close after a fixed number of frames. Frame callbacks can arrive from
/// several per-window render threads at once, hence the atomic.
struct Gameplay {
    frames: AtomicU32,
}

impl Module for Gameplay {
    fn on_engine_initialized(&self, _engine: Engine<'_>) -> bool {
        eprintln!("gameplay (Rust plugin): started");
        true
    }

    fn request_render_frame(&self, request: FrameRequest<'_>) -> bool {
        let count = self.frames.fetch_add(1, Ordering::Relaxed) + 1;
        if count == FRAMES_BEFORE_CLOSE {
            let _ = request.request_close();
        }
        true // render the engine's default frame
    }

    fn on_shutdown(&self, _engine: Engine<'_>) {
        eprintln!("gameplay (Rust plugin): shutdown after {} frames", self.frames.load(Ordering::Relaxed));
    }
}

sturdy_plugin!(|plugin: &mut Plugin<'_>| {
    plugin.set_info("rust", "gameplay", env!("CARGO_PKG_VERSION"));
    plugin.add_module("gameplay", ModuleOptions::scene(), Gameplay { frames: AtomicU32::new(0) })
});
