//! WebGPU overlay probe (see Cargo.toml). Run with `cargo run -p webgpu_overlay`; set `STURDY_PROBE_HDR=1` for HDR.

use std::path::PathBuf;
use std::process::ExitCode;
use std::time::{Duration, Instant};

use sturdy::device::{Backend, DeviceRequest, SwitchOutcome};
use sturdy::prelude::*;
use sturdy::render::{PresentationSettings, SurfaceHandle};
use sturdy::ui::{self, Direction, ElementDesc, Sizing, TextStyle};

const ICON_SVG: &str = r##"<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M12 8v5"/></svg>"##;

struct Probe {
    started: Instant,
    frames: u64,
    icon: PathBuf,
    configured: bool,
}

impl GameLogic for Probe {
    fn init(&mut self, _engine: &mut Engine<'_>) -> Result<(), Error> {
        let icon = self.icon.to_string_lossy().into_owned();
        ui::install_draw_hook(move |ui| {
            let _root = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Grow).direction(Direction::TopToBottom).child_gap(8));
            ui.text("WebGPU overlay probe", &TextStyle::default());
            // Odd sizes on purpose: 14, 18 and 22 are not multiples of the 4x4 compression block.
            for size in [14.0_f32, 18.0, 22.0, 24.0] {
                if let Ok(texture) = ui.load_svg(&icon, size) {
                    ui.svg(&ElementDesc::new().width(Sizing::Fixed(size)).height(Sizing::Fixed(size)), texture);
                }
            }
        });
        Ok(())
    }

    fn frame(&mut self, engine: &mut Engine<'_>, frame: &Frame) -> Option<FrameDescription> {
        if !self.configured {
            self.configured = true;
            let switch = engine.switch_device(&DeviceRequest::new().backend(Backend::WebGpu));
            println!("[webgpu_overlay] switch to WebGPU: {:?}", switch.outcome);
            if matches!(switch.outcome, SwitchOutcome::Failed(_)) {
                if let Some(mut window) = engine.primary_window_control() {
                    window.request_close();
                }
            }
            let surface = SurfaceHandle::from_window_id(frame.window_id);
            engine.set_transparent(surface, true);
            let hdr = std::env::var_os("STURDY_PROBE_HDR").is_some();
            let settings = PresentationSettings { transparent_composition: true, hdr_enabled: hdr, ..engine.presentation_settings(surface) };
            println!("[webgpu_overlay] presentation: {:?}", engine.set_presentation_settings(surface, settings));
        }
        self.frames += 1;
        if self.started.elapsed() > Duration::from_secs(4) {
            println!("[webgpu_overlay] drew {} frames", self.frames);
            if let Some(mut window) = engine.primary_window_control() {
                window.request_close();
            }
        }
        Some(FrameDescription::ui_only())
    }
}

fn main() -> ExitCode {
    let dir = std::env::temp_dir().join("webgpu_overlay_probe");
    let _ = std::fs::create_dir_all(&dir);
    let icon = dir.join("probe.svg");
    let _ = std::fs::write(&icon, ICON_SVG);
    let config = RuntimeConfig::new("webgpu_overlay").size(480, 240).transparent(true).transparent_composition(true);
    sturdy::run(config, Probe { started: Instant::now(), frames: 0, icon, configured: false })
}
