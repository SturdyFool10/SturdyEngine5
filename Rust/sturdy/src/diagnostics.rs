//! Native-GPU-handle interop, extended GPU inventory/capabilities, window diagnostics, a
//! process-wide log sink, and a minimal binding to the engine's worker-pool scheduler.
//!
//! Everything tied to a running engine hangs off [`Diagnostics`], borrowed the same way
//! [`Engine`](crate::Engine) and [`Input`](crate::Input) are (only valid inside a
//! [`GameLogic`](crate::GameLogic) callback). The log sink and the task scheduler are process-wide
//! and outlive any single engine instance, so [`add_log_sink`] and [`spawn`] are free functions
//! instead.
//!
//! Reached via [`Engine::diagnostics`](crate::Engine::diagnostics), like [`Input`](crate::Input).

use core::pin::Pin;

use cxx::UniquePtr;
use sturdy_sys::EngineView;
use sturdy_sys::diagnostics::ffi;

/// Borrowed access to the diagnostics not already covered by [`Engine`](crate::Engine) (frame
/// timing, [`Engine::gpu`](crate::Engine::gpu)) or [`Input`](crate::Input).
///
/// Only exists inside a `GameLogic` callback and borrows from it, same scoping rule as `Engine`.
pub struct Diagnostics<'a> {
    view: Pin<&'a mut EngineView>,
}

impl<'a> Diagnostics<'a> {
    pub(crate) fn new(view: Pin<&'a mut EngineView>) -> Self {
        Self { view }
    }

    fn view(&self) -> &EngineView {
        &self.view
    }

    fn view_mut(&mut self) -> Pin<&mut EngineView> {
        self.view.as_mut()
    }

    // -- Native GPU handle interop -----------------------------------------------------------

    /// Raw Vulkan handles (`VkInstance`/`VkPhysicalDevice`/`VkDevice`/`VkQueue`) for the engine's
    /// active device, for a caller doing its own GPU work against the same device (e.g. with
    /// `ash`). `None` unless the active backend is Vulkan *and* the engine was started with
    /// native access enabled.
    ///
    /// Every handle is **borrowed**: owned by the engine's renderer, must not be destroyed, and
    /// must not be used once this callback returns or the renderer is torn down.
    pub fn native_vulkan(&mut self) -> Option<NativeVulkanHandles> {
        let mut out = ffi::NativeVulkanHandles::default();
        ffi::native_vulkan_handles(self.view_mut(), &mut out).then_some(NativeVulkanHandles {
            instance: out.instance,
            physical_device: out.physical_device,
            device: out.device,
            graphics_queue: out.graphics_queue,
            graphics_queue_family: out.graphics_queue_family,
        })
    }

    /// Raw D3D12 handles for the engine's active device (Windows/D3D12 backend only). Same
    /// borrowed-lifetime and opt-in rules as [`native_vulkan`](Self::native_vulkan).
    pub fn native_d3d12(&mut self) -> Option<NativeD3D12Handles> {
        let mut out = ffi::NativeD3D12Handles::default();
        ffi::native_d3d12_handles(self.view_mut(), &mut out).then_some(NativeD3D12Handles {
            factory: out.factory,
            adapter: out.adapter,
            device: out.device,
            graphics_queue: out.graphics_queue,
        })
    }

    /// Raw WebGPU handles (`WGPUInstance`/`WGPUAdapter`/`WGPUDevice`/`WGPUQueue`) for the engine's
    /// active device (WebGPU/Dawn backend only, i.e. the `webgpu` Cargo feature). Same
    /// borrowed-lifetime and opt-in rules as [`native_vulkan`](Self::native_vulkan) — `None`
    /// unless the active backend is WebGPU *and* the engine was started with native access
    /// enabled. Only the WebGPU objects themselves are exposed (not whatever native API Dawn
    /// chose underneath, e.g. Vulkan on native or nothing at all on Web) — cast each field to its
    /// documented `webgpu.h` type.
    pub fn native_webgpu(&mut self) -> Option<NativeWebGpuHandles> {
        let mut out = ffi::NativeWebGpuHandles::default();
        ffi::native_webgpu_handles(self.view_mut(), &mut out).then_some(NativeWebGpuHandles {
            instance: out.instance,
            adapter: out.adapter,
            device: out.device,
            queue: out.queue,
        })
    }

    // -- Extended GPU inventory & capabilities -------------------------------------------------

    /// Capability flags of the engine's currently selected GPU/backend (multithreaded command
    /// recording, async compute, ray tracing, mesh shaders, bindless, timeline semaphores, and
    /// the resolved frames-in-flight count).
    pub fn gpu_capabilities(&self) -> GpuCapabilities {
        let mut out = ffi::GpuCapabilities::default();
        ffi::gpu_capabilities(self.view(), &mut out);
        GpuCapabilities {
            multithreaded_command_recording: out.multithreaded_command_recording,
            async_compute: out.async_compute,
            raytracing: out.raytracing,
            mesh_shaders: out.mesh_shaders,
            bindless: out.bindless,
            timeline_semaphores: out.timeline_semaphores,
            max_frames_in_flight: out.max_frames_in_flight,
        }
    }

    /// Every GPU the engine discovered at startup, not just the one it selected — see
    /// [`Engine::gpu`](crate::Engine::gpu) for the selected device.
    pub fn gpu_inventory(&self) -> Vec<PhysicalGpu> {
        let view = self.view();
        let count = ffi::gpu_inventory_count(view);
        let mut gpus = Vec::with_capacity(count as usize);
        let mut out = ffi::PhysicalGpuInfo::default();
        for index in 0..count {
            if ffi::gpu_inventory_get(view, index, &mut out) {
                gpus.push(PhysicalGpu {
                    name: out.name.clone(),
                    vendor: out.vendor.clone(),
                    device_type: out.device_type.clone(),
                    vendor_id: out.vendor_id,
                    device_id: out.device_id,
                    physical_device_id: out.physical_device_id.clone(),
                    preference_score: out.preference_score,
                });
            }
        }
        gpus
    }

    // -- Window diagnostics ---------------------------------------------------------------------

    /// Number of windows the engine currently manages.
    pub fn window_count(&self) -> u32 {
        ffi::window_count(self.view())
    }

    /// Diagnostic snapshot of the window at `index` (position, opacity, mouse-lock state, focus —
    /// whatever [`Frame`](crate::Frame) does not already carry per-frame).
    pub fn window(&self, index: u32) -> Option<WindowInfo> {
        let mut out = ffi::WindowInfo::default();
        ffi::window_snapshot(self.view(), index, &mut out).then(|| WindowInfo::from_ffi(&out))
    }

    /// Diagnostic snapshot of the primary window, if the engine has one.
    pub fn primary_window(&self) -> Option<WindowInfo> {
        let mut out = ffi::WindowInfo::default();
        ffi::window_primary(self.view(), &mut out).then(|| WindowInfo::from_ffi(&out))
    }

    /// Raw native handles of the primary window, for embedding a third-party renderer/overlay or
    /// calling platform APIs on it. `None` if there is no primary window or the platform can't
    /// report them. Pointers are **borrowed** — valid only while the window lives.
    pub fn native_window_handle(&mut self) -> Option<NativeWindowHandle> {
        let mut out = ffi::NativeWindowHandleInfo::default();
        ffi::window_native_handle(self.view_mut(), &mut out).then(|| NativeWindowHandle {
            system: match out.system {
                ffi::NativeWindowSystem::Win32 => NativeWindowSystem::Win32,
                ffi::NativeWindowSystem::X11 => NativeWindowSystem::X11,
                ffi::NativeWindowSystem::Wayland => NativeWindowSystem::Wayland,
                ffi::NativeWindowSystem::Cocoa => NativeWindowSystem::Cocoa,
                ffi::NativeWindowSystem::Web => NativeWindowSystem::Web,
                _ => NativeWindowSystem::Unknown,
            },
            display: out.display,
            window: out.window,
            canvas_selector: (!out.canvas_selector.is_empty()).then_some(out.canvas_selector),
        })
    }

    /// Per-pass timings of the last completed frame of `window_id`'s surface (the primary window's
    /// id is `Frame::window_id`): GPU pass times plus CPU pass and stage times. The engine only
    /// collects them while the frame's render graph has `debug_overlay.enabled` set (its text can be
    /// turned off with `draw_text: false`), and GPU times need timestamp-query support.
    pub fn frame_timings(&self, window_id: u64) -> FrameTimings {
        let t = ffi::frame_timings(self.view(), window_id);
        let list = |v: Vec<ffi::TimingEntry>| v.into_iter().map(|e| (e.name, e.milliseconds)).collect::<Vec<_>>();
        FrameTimings { has_data: t.has_data, gpu_passes: list(t.gpu_passes), cpu_passes: list(t.cpu_passes), cpu_stages: list(t.cpu_stages) }
    }

    /// Whether the backend can present on a thread of the renderer's choosing.
    pub fn backend_allows_async_presentation(&self) -> bool {
        ffi::backend_allows_async_presentation(self.view())
    }

    /// Diagnostic snapshot of the window matching `window_id` (the same id
    /// [`Frame::window_id`](crate::Frame::window_id) reports), if it is still managed.
    pub fn find_window(&self, window_id: u64) -> Option<WindowInfo> {
        let mut out = ffi::WindowInfo::default();
        ffi::window_find(self.view(), window_id, &mut out).then(|| WindowInfo::from_ffi(&out))
    }
}

/// Per-pass timings of one frame, in milliseconds, as `(name, ms)` pairs.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct FrameTimings {
    /// `false` until the first frame of the surface completed.
    pub has_data: bool,
    pub gpu_passes: Vec<(String, f64)>,
    pub cpu_passes: Vec<(String, f64)>,
    pub cpu_stages: Vec<(String, f64)>,
}

/// Which windowing system a [`NativeWindowHandle`] belongs to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NativeWindowSystem {
    Unknown,
    Win32,
    X11,
    Wayland,
    Cocoa,
    Web,
}

/// Native window handles; see [`Diagnostics::native_window_handle`]. What `display`/`window` hold
/// depends on `system`: X11 `Display*` / `Window` id, Wayland `wl_display*` / `wl_surface*`, Win32
/// `HINSTANCE` / `HWND`, Cocoa `NSWindow*`. On Web, `canvas_selector` names the canvas instead.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NativeWindowHandle {
    pub system: NativeWindowSystem,
    pub display: u64,
    pub window: u64,
    pub canvas_selector: Option<String>,
}

/// Raw Vulkan handles for the engine's active device. See [`Diagnostics::native_vulkan`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NativeVulkanHandles {
    /// `VkInstance`.
    pub instance: u64,
    /// `VkPhysicalDevice`.
    pub physical_device: u64,
    /// `VkDevice`.
    pub device: u64,
    /// The engine's own primary graphics `VkQueue`.
    pub graphics_queue: u64,
    /// Queue family index backing `graphics_queue`.
    pub graphics_queue_family: u32,
}

/// Raw D3D12 handles for the engine's active device. See [`Diagnostics::native_d3d12`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NativeD3D12Handles {
    /// `IDXGIFactory6*`.
    pub factory: u64,
    /// `IDXGIAdapter4*`.
    pub adapter: u64,
    /// `ID3D12Device*`.
    pub device: u64,
    /// `ID3D12CommandQueue*` (primary graphics queue).
    pub graphics_queue: u64,
}

/// Raw WebGPU handles for the engine's active device. See [`Diagnostics::native_webgpu`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NativeWebGpuHandles {
    /// `WGPUInstance`.
    pub instance: u64,
    /// `WGPUAdapter`.
    pub adapter: u64,
    /// `WGPUDevice`.
    pub device: u64,
    /// `WGPUQueue`, the one queue every WebGPU device exposes.
    pub queue: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GpuCapabilities {
    pub multithreaded_command_recording: bool,
    pub async_compute: bool,
    pub raytracing: bool,
    pub mesh_shaders: bool,
    pub bindless: bool,
    pub timeline_semaphores: bool,
    pub max_frames_in_flight: u32,
}

/// One entry of the engine's full GPU inventory. See [`Diagnostics::gpu_inventory`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PhysicalGpu {
    pub name: String,
    pub vendor: String,
    pub device_type: String,
    pub vendor_id: u32,
    pub device_id: u32,
    pub physical_device_id: String,
    /// The same discrete-GPU-preferring heuristic the engine's own adapter selection scores
    /// candidates with; higher is more preferred by that heuristic. Not a hardware benchmark.
    pub preference_score: i64,
}

/// Diagnostic snapshot of one window. See [`Diagnostics::window`].
///
/// Monitor details (refresh rate, scale, HDR per display) are in
/// [`PrimaryWindow::displays`](crate::window::PrimaryWindow::displays); this snapshot carries the window's own state.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WindowInfo {
    pub window_id: u64,
    /// Window size in logical pixels.
    pub size: glam::UVec2,
    /// Framebuffer (drawable surface) size in physical pixels — differs from `size` under
    /// high-DPI scaling.
    pub framebuffer_size: glam::UVec2,
    pub position: glam::IVec2,
    pub opacity: f32,
    pub mouse_locked: bool,
    pub focused: bool,
    /// Updated from window events; reads true right after the user (or the OS) maximizes the window.
    pub maximized: bool,
    pub minimized: bool,
    /// Window-system UI scale (1.0 = 100%); changes when the window moves to a display with another scale, so a UI
    /// that lays out in logical pixels re-reads it each frame.
    pub content_scale: f32,
    /// The OS light/dark preference.
    pub system_theme: crate::window::SystemTheme,
}

impl WindowInfo {
    fn from_ffi(info: &ffi::WindowInfo) -> Self {
        Self {
            window_id: info.window_id,
            size: glam::uvec2(info.width, info.height),
            framebuffer_size: glam::uvec2(info.framebuffer_width, info.framebuffer_height),
            position: glam::ivec2(info.position_x, info.position_y),
            opacity: info.opacity,
            mouse_locked: info.mouse_locked,
            focused: info.focused,
            maximized: info.maximized,
            minimized: info.minimized,
            content_scale: info.content_scale,
            system_theme: match info.system_theme {
                1 => crate::window::SystemTheme::Light,
                2 => crate::window::SystemTheme::Dark,
                _ => crate::window::SystemTheme::Unknown,
            },
        }
    }
}

// -- Process-wide log sink ----------------------------------------------------------------------

pub use ffi::LogLevel;

/// Registers `callback` to run for every subsequent engine log message, on top of the engine's
/// own console/file sinks. Safe to call from any thread, at any point, including before the
/// engine starts.
///
/// `callback` may run from arbitrary engine threads, possibly concurrently, hence `Send + Sync`;
/// a panic inside it aborts the process (it cannot unwind back through the C++ logging call that
/// invoked it).
pub fn add_log_sink(callback: impl Fn(LogLevel, &str) + Send + Sync + 'static) -> LogSinkHandle {
    let sink = Box::new(sturdy_sys::diagnostics::LogSink(Box::new(callback)));
    LogSinkHandle(ffi::log_add_sink(sink))
}

/// Handle returned by [`add_log_sink`]; unregisters the sink when [`remove`](Self::remove) is
/// called. Dropping the handle without calling `remove` leaves the sink installed for the rest of
/// the process's life.
#[derive(Debug)]
pub struct LogSinkHandle(u64);

impl LogSinkHandle {
    /// Unregisters the sink.
    pub fn remove(self) {
        ffi::log_remove_sink(self.0);
    }
}

// -- Process-wide task scheduler ------------------------------------------------------------------

/// Number of worker threads in the engine's task scheduler.
pub fn worker_count() -> u32 {
    ffi::scheduler_worker_count()
}

/// Whether the scheduler is currently running. It is started/stopped by the engine's own
/// application lifecycle, so this is a diagnostic, not something a caller starts/stops; it is
/// expected to be `true` for the whole time any `GameLogic` callback can run.
pub fn is_running() -> bool {
    ffi::scheduler_is_running()
}

/// Runs `work` once on the engine's worker pool and returns a handle to poll/wait on it.
///
/// `work` may run on any worker thread, so it must be `Send`; a panic inside it aborts the
/// process. This is a thin binding to `SFT::Async::Scheduler::spawn`, not a general async
/// runtime: there is no cancellation, and `work` always runs to completion once spawned.
pub fn spawn(work: impl FnOnce() + Send + 'static) -> TaskHandle {
    spawn_with_weight(work, false)
}

/// Like [`spawn`], but hints the scheduler that `work` is longer-running ("heavy") so it can be
/// balanced differently from short "light" work.
pub fn spawn_heavy(work: impl FnOnce() + Send + 'static) -> TaskHandle {
    spawn_with_weight(work, true)
}

fn spawn_with_weight(work: impl FnOnce() + Send + 'static, heavy: bool) -> TaskHandle {
    let task = Box::new(sturdy_sys::diagnostics::SchedulerTask(Some(Box::new(work))));
    TaskHandle(ffi::scheduler_spawn(task, heavy))
}

/// Queues `work` to run on the main (window/event-loop) thread at the engine's next per-frame pump.
/// Use it for anything that must happen on that thread (platform/windowing calls) from a worker task.
pub fn run_on_main_thread(work: impl FnOnce() + Send + 'static) {
    ffi::run_on_main_thread(Box::new(sturdy_sys::diagnostics::SchedulerTask(Some(Box::new(work)))));
}

/// Logical core indices ranked fastest-first by the engine's CPU topology detection.
pub fn ranked_logical_cores() -> Vec<u32> {
    ffi::ranked_logical_cores()
}

/// Physical core indices ranked fastest-first.
pub fn ranked_physical_cores() -> Vec<u32> {
    ffi::ranked_physical_cores()
}

/// A task spawned with [`spawn`]/[`spawn_heavy`].
pub struct TaskHandle(UniquePtr<ffi::TaskHandle>);

impl TaskHandle {
    /// Whether the task has finished.
    pub fn is_done(&self) -> bool {
        ffi::task_handle_is_done(&self.0)
    }

    /// Blocks the calling thread until the task completes.
    pub fn wait(&self) {
        ffi::task_handle_wait(&self.0);
    }
}
