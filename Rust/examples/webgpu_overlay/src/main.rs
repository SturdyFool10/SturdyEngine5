//! WebGPU overlay probe (see Cargo.toml). Run with `cargo run -p webgpu_overlay`; set `STURDY_PROBE_HDR=1` for HDR.

use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use sturdy::assets::{RendererTextureHandle, TextureColorSpace, TextureKind, TexturePixelFormat};

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
    textures: Arc<Mutex<Vec<(RendererTextureHandle, f32, f32)>>>,
    churn: Vec<sturdy::assets::Asset>,
    warm_frames: u64,
}

impl GameLogic for Probe {
    fn init(&mut self, engine: &mut Engine<'_>) -> Result<(), Error> {
        // STURDY_PROBE_FONTS=<dir>: the launcher's font stack (Maple Mono, Maple CN fallback, Noto Color Emoji, OpenType
        // features, weight variation), which is where its text differs from the default font.
        if let Some(dir) = std::env::var_os("STURDY_PROBE_FONTS") {
            let dir = PathBuf::from(dir);
            let read = |name: &str| std::fs::read(dir.join(name)).expect("font file");
            let stack = sturdy::ui::FontStack::new(sturdy::ui::FontSource::Bytes(read("MapleMono-NF-Regular.ttf")))
                .fallback(sturdy::ui::FontSource::Bytes(read("MapleMono-NF-CN-Regular.ttf")))
                .variation(sturdy::ui::FontVariation::weight(400.0));
            let mut stack = stack;
            if std::env::var_os("STURDY_PROBE_EMOJI").is_some() {
                stack = stack.emoji(sturdy::ui::FontSource::Bytes(read("NotoColorEmoji.ttf")));
            }
            stack.features = "kern,liga,calt,onum,pnum".to_owned();
            if let Err(err) = engine.register_ui_font_stack(0, &stack) {
                println!("[webgpu_overlay] font stack failed: {err}");
            }
        }
        let icon = self.icon.to_string_lossy().into_owned();
        let textures = self.textures.clone();
        let mut probe_states = (sturdy::widgets::TextEditState::with_text(""), sturdy::widgets::TextEditState::with_text(""));
        let mut probe_frame = 0u32;
        ui::install_draw_hook(move |ui| {
            // STURDY_PROBE_TEXT: two focused single-line fields, typed into by injection; prints what each ends up holding.
            if std::env::var_os("STURDY_PROBE_TEXT").is_some() {
                probe_frame += 1;
                if std::env::var_os("STURDY_PROBE_REAL").is_some() && (29..36).contains(&probe_frame) {
                    println!("[webgpu_overlay] hook start frame {probe_frame}: ui_typed={:?}", ui.text_input().typed_text);
                }
                if probe_frame == 5 {
                    probe_states.0.set_text("Mg");
                    probe_states.1.set_text("Mg");
                }
                if probe_frame == 20 {
                    probe_states.0.set_focused(true);
                    probe_states.1.set_focused(true);
                }
                if (30..33).contains(&probe_frame) && std::env::var_os("STURDY_PROBE_REAL").is_none() {
                    ui.type_text("7");
                }
                let style = sturdy::widgets::TextInputStyle::default();
                let a = ui.text_field(&ElementDesc::new().id("probe::plain".into()).width(Sizing::Fixed(200.0)).height(Sizing::Fixed(40.0)).padding(sturdy::ui::Padding::symmetric(6, 0)).align(sturdy::ui::ChildAlignment::new(sturdy::ui::AlignX::Left, sturdy::ui::AlignY::Center)), &style, &mut probe_states.0, "", true);
                let b = ui.text_field(&ElementDesc::new().id("probe::placeholder".into()).width(Sizing::Fixed(200.0)).height(Sizing::Fixed(40.0)), &style, &mut probe_states.1, "250Mbps", true);
                let real = std::env::var_os("STURDY_PROBE_REAL").is_some();
                let typed_now = ui.text_input().typed_text;
                if real && (30..33).contains(&probe_frame) {
                    ui.type_text("7");
                }
                if real && (!typed_now.is_empty() || a.changed || b.changed) {
                    println!("[webgpu_overlay] REAL frame {probe_frame}: ui_typed={typed_now:?} plain={:?} placeholder={:?}", probe_states.0.text(), probe_states.1.text());
                }
                if !real && (29..36).contains(&probe_frame) {
                    println!("[webgpu_overlay] text frame {probe_frame}: plain={:?} (changed {}) placeholder={:?} (changed {}) typed={:?}", probe_states.0.text(), a.changed, probe_states.1.text(), b.changed, ui.engine().input().text());
                }
            }
            let _root = ui.element(
                &ElementDesc::new()
                    .width(Sizing::Grow)
                    .height(Sizing::Grow)
                    .direction(Direction::TopToBottom)
                    .child_gap(8)
                    .padding(sturdy::ui::Padding::all(16))
                    // Half transparent: if the surface really is translucent the desktop shows through.
                    .background(sturdy::glam::Vec4::new(0.05, 0.1, 0.3, 0.5)),
            );
            ui.text("WebGPU overlay probe", &TextStyle::default());
            let line = if std::env::var_os("STURDY_PROBE_NO_EMOJI_CHAR").is_some() { "僵尸入侵 100 天（高版本惊变重制） Fabulously Optimized" } else { "僵尸入侵 100 天（高版本惊变重制） 🎮 Fabulously Optimized" };
            ui.text(line, &TextStyle::default());
            // Lots of distinct glyph/size pairs so the atlas has to take many uploads at once (and more later).
            let many = std::env::var_os("STURDY_PROBE_MANY_GLYPHS").is_some();
            let sizes: Vec<u16> = if many { (8u16..=48).collect() } else { vec![11u16, 13, 15, 17, 19, 22, 26] };
            for size in sizes {
                ui.text("Fabulously Optimized: the quick brown fox jumps over 144 lazy dogs", &TextStyle { font_size: size, ..TextStyle::default() });
            }
            // Odd sizes on purpose: 14, 18 and 22 are not multiples of the 4x4 compression block.
            for size in [14.0_f32, 18.0, 22.0, 24.0] {
                if let Ok(texture) = ui.load_svg(&icon, size) {
                    ui.svg(&ElementDesc::new().width(Sizing::Fixed(size)).height(Sizing::Fixed(size)), texture);
                }
            }
            // STURDY_PROBE_DRAWS: hundreds of alternating-texture images, so one frame needs far more than the first
            // 256 push-constant slices (a Discover-sized page does).
            if std::env::var_os("STURDY_PROBE_DRAWS").is_some() {
                if let Ok(list) = textures.lock() {
                    if list.len() >= 2 {
                        let _grid = ui.element(&ElementDesc::new().width(Sizing::Fixed(440.0)).direction(Direction::LeftToRight).child_gap(2));
                        for i in 0..900 {
                            let (texture, _, _) = list[i % 2];
                            ui.image(&ElementDesc::new().width(Sizing::Fixed(6.0)).height(Sizing::Fixed(6.0)), texture);
                        }
                    }
                }
            }
            // STURDY_PROBE_CLIPS: hundreds of clipped boxes, each its own scissor and so its own draw batches (a list of
            // cards in a scroll area does this), which is what stresses per-draw costs such as push constants.
            if std::env::var_os("STURDY_PROBE_CLIPS").is_some() {
                if let Ok(list) = textures.lock() {
                    if let Some((texture, _, _)) = list.first().copied() {
                        let _grid = ui.element(&ElementDesc::new().width(Sizing::Fixed(460.0)).direction(Direction::LeftToRight).child_gap(2));
                        for i in 0..700 {
                            let _cell = ui.element(
                                &ElementDesc::new()
                                    .width(Sizing::Fixed(8.0))
                                    .height(Sizing::Fixed(8.0))
                                    .clip(sturdy::ui::Clip::both())
                                    .background(sturdy::glam::Vec4::new(0.2 + (i % 5) as f32 * 0.1, 0.3, 0.4, 1.0)),
                            );
                            ui.image(&ElementDesc::new().width(Sizing::Fixed(6.0)).height(Sizing::Fixed(6.0)), texture);
                        }
                    }
                }
            }
            // Pictures like the launcher's (odd sizes, mipmapped, uncompressed): avatars, icons, cape faces.
            if let Ok(list) = textures.lock() {
                let _row = ui.element(&ElementDesc::new().direction(Direction::LeftToRight).child_gap(6));
                for (texture, w, h) in list.iter() {
                    ui.image(&ElementDesc::new().width(Sizing::Fixed(*w / 2.0)).height(Sizing::Fixed(*h / 2.0)), *texture);
                }
            }
        });
        Ok(())
    }

    fn frame(&mut self, engine: &mut Engine<'_>, frame: &Frame) -> Option<FrameDescription> {
        if !self.configured {
            self.configured = true;
            let backend = if std::env::var("STURDY_PROBE_BACKEND").is_ok_and(|v| v == "vulkan") { Backend::Vulkan } else { Backend::WebGpu };
            let switch = engine.switch_device(&DeviceRequest::new().backend(backend));
            println!("[webgpu_overlay] switch to WebGPU: {:?}", switch.outcome);
            if matches!(switch.outcome, SwitchOutcome::Failed(_)) {
                if let Some(mut window) = engine.primary_window_control() {
                    window.request_close();
                }
            }
            // Upload a spread of picture sizes through the same call the launcher uses.
            for (w, h) in [(70u32, 112u32), (500, 500), (96, 96), (64, 64), (128, 128), (13, 7)] {
                let pixels: Vec<u8> = (0..w * h).flat_map(|i| [(i % 251) as u8, (i % 127) as u8 * 2, 200, 255]).collect();
                let mut assets = engine.assets();
                match assets.create_texture(&pixels, w, h, TexturePixelFormat::Rgba8, TextureColorSpace::Srgb, TextureKind::ColorAlpha, "probe picture", false, true) {
                    Ok(asset) => {
                        if let Ok(handle) = assets.texture_handle(asset) {
                            if let Ok(mut list) = self.textures.lock() {
                                list.push((handle, w as f32, h as f32));
                            }
                        }
                    }
                    Err(err) => println!("[webgpu_overlay] texture {w}x{h} failed: {err:?}"),
                }
            }
            let surface = SurfaceHandle::from_window_id(frame.window_id);
            engine.set_transparent(surface, true);
            let hdr = std::env::var_os("STURDY_PROBE_HDR").is_some();
            // STURDY_PROBE_BENCH: uncapped, no vsync, so the frame count measures CPU/driver cost rather than the display.
            let bench = std::env::var_os("STURDY_PROBE_BENCH").is_some();
            let mut settings = PresentationSettings { transparent_composition: true, hdr_enabled: hdr, ..engine.presentation_settings(surface) };
            if bench {
                settings.vsync = sturdy::VSync::Off;
                settings.variable_refresh = sturdy::VariableRefresh::Disabled;
                settings.frame_rate_limit = sturdy::render::FrameRateLimit::Unlimited;
            }
            println!("[webgpu_overlay] presentation: {:?}", engine.set_presentation_settings(surface, settings));
        }
        self.frames += 1;
        if self.warm_frames == 0 && self.started.elapsed() > Duration::from_secs(3) {
            self.warm_frames = self.frames;
        }
        // Stress (STURDY_PROBE_STRESS=1): what the launcher does at runtime. Flip presentation settings and churn textures.
        if std::env::var_os("STURDY_PROBE_STRESS").is_some() && self.frames % 15 == 0 {
            let surface = SurfaceHandle::from_window_id(frame.window_id);
            let step = self.frames / 15;
            let mut settings = engine.presentation_settings(surface);
            settings.hdr_enabled = step % 2 == 0;
            settings.transparent_composition = step % 3 != 0;
            engine.set_transparent(surface, settings.transparent_composition);
            let result = engine.set_presentation_settings(surface, settings);
            if let Err(err) = result {
                println!("[webgpu_overlay] presentation step {step} failed: {err}");
            }
            let mut assets = engine.assets();
            if self.churn.len() > 24 {
                let old = self.churn.remove(0);
                let _ = assets.unload(old);
            }
            let (w, h) = (96 + (step % 7) as u32 * 3, 96 + (step % 5) as u32 * 5);
            let pixels = vec![(step % 255) as u8; (w * h * 4) as usize];
            if let Ok(asset) = assets.create_texture(&pixels, w, h, TexturePixelFormat::Rgba8, TextureColorSpace::Srgb, TextureKind::ColorAlpha, "churn", false, true) {
                self.churn.push(asset);
            }
        }
        let seconds = std::env::var("STURDY_PROBE_SECONDS").ok().and_then(|v| v.parse().ok()).unwrap_or(4);
        if self.started.elapsed() > Duration::from_secs(seconds) {
            let measured = self.started.elapsed().as_secs_f64() - 3.0;
            println!("[webgpu_overlay] drew {} frames; {:.1} fps over the last {:.1}s", self.frames, (self.frames - self.warm_frames) as f64 / measured.max(0.001), measured);
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
    sturdy::run(config, Probe { started: Instant::now(), frames: 0, icon, configured: false, textures: Arc::new(Mutex::new(Vec::new())), churn: Vec::new(), warm_frames: 0 })
}
