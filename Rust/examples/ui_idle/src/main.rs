//! On-demand rendering check (see `sturdy::app`).
//!
//! Phase 1 (warm-up, 2 s): the window opens and settles.
//! Phase 2 (5 s): nothing happens except a background thread calling `request_redraw()` every 500 ms. The app must
//! render about 10 frames (not ~300 like a continuous loop) and use little CPU.
//! Phase 3: `request_redraw_after(1 s)` from the frame callback must produce one more frame about a second later.
//!
//! Prints a verdict and exits with status 1 when a check fails.

use std::process::ExitCode;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use sturdy::app::FrameMode;
use sturdy::prelude::*;
use sturdy::ui::{self, Direction, ElementDesc, Sizing, TextStyle};

/// CPU time (user + system) this process has used, from /proc (Linux only; zero elsewhere).
fn cpu_time() -> Duration {
    let Ok(stat) = std::fs::read_to_string("/proc/self/stat") else { return Duration::ZERO };
    // Fields after the parenthesised command name; utime and stime are fields 14 and 15 (1-based).
    let after = stat.rsplit_once(')').map(|(_, rest)| rest).unwrap_or("");
    let fields: Vec<&str> = after.split_whitespace().collect();
    let ticks = |i: usize| fields.get(i).and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
    // after ')' the first field is the state (field 3), so utime (14) is index 11 and stime (15) index 12.
    let total_ticks = ticks(11) + ticks(12);
    let hz = 100u64; // _SC_CLK_TCK is 100 on every Linux this runs on
    Duration::from_millis(total_ticks * 1000 / hz)
}

struct Check {
    start: Instant,
    frames: Arc<AtomicU64>,
    phase2_start: Option<(Instant, u64, Duration)>,
    phase2_result: Option<(u64, f64)>,
    phase3_requested_at: Option<(Instant, u64)>,
    failed: Arc<std::sync::atomic::AtomicBool>,
    done: bool,
}

impl GameLogic for Check {
    fn init(&mut self, engine: &mut Engine<'_>) -> Result<(), Error> {
        engine.set_frame_mode(FrameMode::OnDemand);
        let frames = self.frames.clone();
        ui::install_draw_hook(move |ui| {
            let _root = ui.element(&ElementDesc::new().width(Sizing::Grow).height(Sizing::Grow).direction(Direction::TopToBottom));
            ui.text(&format!("on-demand frame {}", frames.load(Ordering::Relaxed)), &TextStyle::default());
        });
        let redraw = engine.redraw_handle();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_secs(2));
            for _ in 0..10 {
                std::thread::sleep(Duration::from_millis(500));
                redraw.request_redraw();
            }
        });
        Ok(())
    }

    fn frame(&mut self, engine: &mut Engine<'_>, _frame: &Frame) -> Option<FrameDescription> {
        let n = self.frames.fetch_add(1, Ordering::Relaxed) + 1;
        let t = self.start.elapsed();

        if self.phase2_start.is_none() && t >= Duration::from_secs(2) {
            self.phase2_start = Some((Instant::now(), n, cpu_time()));
        }
        if let Some((at, frames_at, cpu_at)) = self.phase2_start {
            // The background thread's last redraw lands ~4.5 s after the first one, so the window closes on it.
            if self.phase2_result.is_none() && at.elapsed() >= Duration::from_millis(4400) {
                let frames = n - frames_at;
                let cpu = (cpu_time() - cpu_at).as_secs_f64() / at.elapsed().as_secs_f64() * 100.0;
                self.phase2_result = Some((frames, cpu));
                // Phase 3: one timed redraw, a second from now.
                self.phase3_requested_at = Some((Instant::now(), n));
                engine.request_redraw_after(Duration::from_secs(1));
            }
        }
        if let (Some((frames, cpu)), Some((asked, frames_at))) = (self.phase2_result, self.phase3_requested_at) {
            if n > frames_at && !self.done {
                let delay = asked.elapsed();
                println!("[ui_idle] idle phase: {frames} frames in 4.4 s (continuous would be ~300), CPU {cpu:.1}%");
                println!("[ui_idle] request_redraw_after(1 s) produced the next frame after {:.0} ms", delay.as_secs_f64() * 1000.0);
                let ok_frames = (8..=20).contains(&frames);
                let ok_cpu = cpu < 10.0;
                let ok_timer = delay >= Duration::from_millis(900) && delay <= Duration::from_millis(1500);
                if !(ok_frames && ok_cpu && ok_timer) {
                    println!("[ui_idle] FAILED: frames_ok={ok_frames} cpu_ok={ok_cpu} timer_ok={ok_timer}");
                    self.failed.store(true, Ordering::Relaxed);
                } else {
                    println!("[ui_idle] all on-demand checks passed");
                }
                self.done = true;
                if let Some(mut window) = engine.primary_window_control() {
                    window.request_close();
                }
            }
        }
        Some(FrameDescription::ui_only())
    }
}

fn main() -> ExitCode {
    let failed = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let check = Check {
        start: Instant::now(),
        frames: Arc::new(AtomicU64::new(0)),
        phase2_start: None,
        phase2_result: None,
        phase3_requested_at: None,
        failed: failed.clone(),
        done: false,
    };
    let code = sturdy::run(RuntimeConfig::new("ui_idle").size(640, 360), check);
    if failed.load(Ordering::Relaxed) { ExitCode::FAILURE } else { code }
}
