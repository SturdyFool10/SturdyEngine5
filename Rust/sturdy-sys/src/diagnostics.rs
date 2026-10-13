//! Native-GPU-handle interop, extended GPU inventory/capabilities, window diagnostics, a
//! process-wide log sink, and a minimal binding to the engine's worker-pool scheduler.
//!
//! Everything tied to a running engine reuses `EngineView` from the root bridge
//! (`crate::ffi::EngineView`). The log sink and the task scheduler are process-wide singletons
//! (spdlog's default logger and `SFT::Async::Scheduler` both live outside any single `Engine`, and
//! the scheduler is already started/stopped by the engine's own application lifecycle), so their
//! functions take no `EngineView` at all.

#[cxx::bridge(namespace = "sturdy_rs::diagnostics")]
pub mod ffi {
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum LogLevel {
        Trace,
        Debug,
        Info,
        Warn,
        Error,
        Critical,
    }

    /// Raw Vulkan handles for the engine's active device.
    ///
    /// Every field is a native handle reinterpreted as an integer (`VkInstance`, `VkPhysicalDevice`,
    /// `VkDevice`, `VkQueue`); the caller is expected to already know what to do with one (e.g. via
    /// `ash` or another Vulkan binding used against the same device). All of them are **borrowed**:
    /// they are owned by the engine's renderer and must not be destroyed, and must not be used after
    /// the `Engine`/its `GameLogic` callback returns or the renderer is torn down. Only populated
    /// when the active backend is Vulkan *and* the running engine was started with native access
    /// enabled (`RendererFeatureRequest::enable_native_access_extension` on the C++ side); otherwise
    /// every accessor here reports unavailable.
    #[derive(Debug, Clone, Copy, Default)]
    struct NativeVulkanHandles {
        /// `VkInstance`.
        instance: u64,
        /// `VkPhysicalDevice`.
        physical_device: u64,
        /// `VkDevice`.
        device: u64,
        /// The engine's own primary graphics `VkQueue`.
        graphics_queue: u64,
        /// Queue family index backing `graphics_queue`.
        graphics_queue_family: u32,
    }

    /// Raw D3D12 handles for the engine's active device (Windows/D3D12 backend only). Same
    /// borrowed-lifetime and opt-in rules as [`NativeVulkanHandles`].
    #[derive(Debug, Clone, Copy, Default)]
    struct NativeD3D12Handles {
        /// `IDXGIFactory6*`.
        factory: u64,
        /// `IDXGIAdapter4*`.
        adapter: u64,
        /// `ID3D12Device*`.
        device: u64,
        /// `ID3D12CommandQueue*` (primary graphics queue).
        graphics_queue: u64,
    }

    /// Raw WebGPU handles for the engine's active device (WebGPU/Dawn backend only, i.e. the
    /// `webgpu` Cargo feature). Same borrowed-lifetime and opt-in rules as
    /// [`NativeVulkanHandles`]/[`NativeD3D12Handles`], except there is only one queue to report —
    /// WebGPU has no separate graphics/compute/transfer queues to distinguish, matching the
    /// reference C API's own `SturdyWebGpuHandles` (`FFI/src/FFI/Sturdy.h`), which this mirrors.
    /// Cast each field to its `webgpu.h` type (`WGPUInstance`/`WGPUAdapter`/`WGPUDevice`/
    /// `WGPUQueue`).
    #[derive(Debug, Clone, Copy, Default)]
    struct NativeWebGpuHandles {
        /// `WGPUInstance`.
        instance: u64,
        /// `WGPUAdapter`.
        adapter: u64,
        /// `WGPUDevice`.
        device: u64,
        /// `WGPUQueue`, the one queue every WebGPU device exposes.
        queue: u64,
    }

    /// Capability flags of the engine's currently selected GPU/backend.
    #[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
    struct GpuCapabilities {
        multithreaded_command_recording: bool,
        async_compute: bool,
        raytracing: bool,
        mesh_shaders: bool,
        bindless: bool,
        timeline_semaphores: bool,
        max_frames_in_flight: u32,
    }

    /// One entry of the engine's full GPU inventory (every GPU it discovered at startup, not just
    /// the one it selected — see `Engine::gpu()` in the `sturdy` crate for the selected device).
    #[derive(Debug, Clone, Default, PartialEq, Eq)]
    struct PhysicalGpuInfo {
        name: String,
        vendor: String,
        device_type: String,
        vendor_id: u32,
        device_id: u32,
        physical_device_id: String,
        /// The same discrete-GPU-preferring heuristic the engine's own adapter selection scores
        /// candidates with (`RHI::score_adapter` under `PowerPreference::HighPerformance`); higher
        /// is more preferred by that heuristic. Not a hardware benchmark score.
        preference_score: i64,
    }

    /// Diagnostic snapshot of one window not already covered by [`crate::ffi::FrameInfo`]:
    /// position, opacity, mouse-lock state and focus. Refreshed once per tick by the engine.
    #[derive(Debug, Clone, Copy, Default, PartialEq)]
    struct WindowInfo {
        window_id: u64,
        width: u32,
        height: u32,
        framebuffer_width: u32,
        framebuffer_height: u32,
        position_x: i32,
        position_y: i32,
        opacity: f32,
        mouse_locked: bool,
        focused: bool,
        maximized: bool,
        minimized: bool,
        /// Window-system UI scale (1.0 = 100%); changes when the window moves between displays.
        content_scale: f32,
        /// 0 unknown, 1 light, 2 dark.
        system_theme: u8,
    }

    /// Mirrors `SFT::WindowManager::NativeWindowSystem`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
    enum NativeWindowSystem {
        #[default]
        Unknown,
        Win32,
        X11,
        Wayland,
        Cocoa,
        Web,
    }

    /// Raw native handles for a window. Both pointers are **borrowed** from the engine's window
    /// and valid only while that window lives (same rules as the GPU handles above). What
    /// `display`/`window` point at depends on `system`: X11 `Display*`/`Window` id, Wayland
    /// `wl_display*`/`wl_surface*`, Win32 `HINSTANCE`/`HWND`, Cocoa `NSWindow*`; on Web,
    /// `canvas_selector` names the canvas instead.
    #[derive(Debug, Clone, Default)]
    struct NativeWindowHandleInfo {
        system: NativeWindowSystem,
        display: u64,
        window: u64,
        canvas_selector: String,
    }

    /// One named timing in milliseconds.
    #[derive(Debug, Clone, Default)]
    struct TimingEntry {
        name: String,
        milliseconds: f64,
    }

    /// `SFT::Renderer::FrameTimingSnapshot` for the last completed frame of a surface.
    #[derive(Debug, Clone, Default)]
    struct FrameTimings {
        has_data: bool,
        gpu_passes: Vec<TimingEntry>,
        cpu_passes: Vec<TimingEntry>,
        cpu_stages: Vec<TimingEntry>,
    }

    extern "Rust" {
        type LogSink;
        type SchedulerTask;

        fn log_sink_call(sink: &LogSink, level: LogLevel, message: &[u8]);
        fn scheduler_task_run(task: &mut SchedulerTask);
    }

    unsafe extern "C++" {
        include!("sturdy_rs/shim.hpp");
        include!("sturdy_rs/diagnostics.hpp");

        /// `SFT::Engine::Engine`, aliasing the root bridge's opaque type. The `#[namespace]`
        /// override is required: without it cxx derives this alias's `ExternType::Id` from *this*
        /// bridge's own namespace (`sturdy_rs::diagnostics::EngineView`) instead of the root
        /// bridge's actual identity (`sturdy_rs::EngineView`), and fails `verify_extern_type`.
        #[namespace = "sturdy_rs"]
        type EngineView = crate::ffi::EngineView;

        /// Wraps `SFT::Async::TaskHandle<void>`; returned by `scheduler_spawn`.
        type TaskHandle;

        // -- Native GPU handle interop -------------------------------------------------------

        fn native_vulkan_available(engine: Pin<&mut EngineView>) -> bool;
        fn native_vulkan_handles(engine: Pin<&mut EngineView>, out: &mut NativeVulkanHandles) -> bool;
        fn native_d3d12_available(engine: Pin<&mut EngineView>) -> bool;
        fn native_d3d12_handles(engine: Pin<&mut EngineView>, out: &mut NativeD3D12Handles) -> bool;
        fn native_webgpu_available(engine: Pin<&mut EngineView>) -> bool;
        fn native_webgpu_handles(engine: Pin<&mut EngineView>, out: &mut NativeWebGpuHandles) -> bool;

        // -- Extended GPU inventory & capabilities ---------------------------------------------

        fn gpu_capabilities(engine: &EngineView, out: &mut GpuCapabilities) -> bool;
        fn gpu_inventory_count(engine: &EngineView) -> u32;
        fn gpu_inventory_get(engine: &EngineView, index: u32, out: &mut PhysicalGpuInfo) -> bool;

        // -- Window diagnostics -----------------------------------------------------------------

        fn window_count(engine: &EngineView) -> u32;
        fn window_snapshot(engine: &EngineView, index: u32, out: &mut WindowInfo) -> bool;
        fn window_primary(engine: &EngineView, out: &mut WindowInfo) -> bool;
        fn window_find(engine: &EngineView, window_id: u64, out: &mut WindowInfo) -> bool;
        /// Per-pass GPU/CPU timings of the last completed frame of `window_id`'s surface.
        fn frame_timings(engine: &EngineView, window_id: u64) -> FrameTimings;
        fn backend_allows_async_presentation(engine: &EngineView) -> bool;
        /// Native handles of the primary window; `false` if there is no primary window or the
        /// platform can't report them.
        fn window_native_handle(engine: Pin<&mut EngineView>, out: &mut NativeWindowHandleInfo) -> bool;

        // -- Process-wide log sink ----------------------------------------------------------------

        /// Registers `sink` to run for every subsequent log message, on top of the engine's own
        /// sinks. Safe from any thread at any point, including before the engine starts. Returns an
        /// id for `log_remove_sink`.
        fn log_add_sink(sink: Box<LogSink>) -> u64;
        /// Unregisters a sink added by `log_add_sink`; an unknown/already-removed id is a no-op.
        fn log_remove_sink(id: u64);

        // -- Process-wide task scheduler ------------------------------------------------------------

        /// Number of worker threads in the engine's task scheduler.
        fn scheduler_worker_count() -> u32;
        /// Whether the scheduler is currently running (it is started/stopped by the engine's own
        /// application lifecycle; this is a diagnostic, not something a caller starts/stops).
        fn scheduler_is_running() -> bool;
        /// Runs `task` once on the scheduler's worker pool. `heavy` hints a longer-running task so
        /// the scheduler can balance it differently from short "light" work.
        fn scheduler_spawn(task: Box<SchedulerTask>, heavy: bool) -> UniquePtr<TaskHandle>;
        /// Queues `task` to run on the main (window/event-loop) thread the next time the engine
        /// pumps it (once per frame). Runs immediately if already on the main thread's pump.
        fn run_on_main_thread(task: Box<SchedulerTask>);
        /// Logical core indices ranked fastest-first by the engine's topology detection.
        fn ranked_logical_cores() -> Vec<u32>;
        /// Physical core indices ranked fastest-first.
        fn ranked_physical_cores() -> Vec<u32>;
        fn task_handle_is_done(handle: &TaskHandle) -> bool;
        /// Blocks the calling thread until the task completes.
        fn task_handle_wait(handle: &TaskHandle);
    }
}

use std::panic::{catch_unwind, AssertUnwindSafe};

/// A panic must never unwind into C++ frames (undefined behavior); report it and abort.
///
/// Mirrors `guard()` in `sturdy/src/runtime.rs`; duplicated here (this crate cannot reach that
/// private fn in the `sturdy` crate) for the two callbacks below, both of which can run on
/// arbitrary engine/worker threads, possibly concurrently.
///
/// `pub(crate)` rather than private: `schedule.rs`'s system trampolines need the exact same
/// abort-on-panic behavior (called from arbitrary scheduler worker threads, possibly
/// concurrently) and reuse this copy rather than triplicating it.
pub(crate) fn guard<R>(what: &str, f: impl FnOnce() -> R) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(value) => value,
        Err(_) => {
            eprintln!("sturdy: `{what}` callback panicked; aborting (cannot unwind through C++)");
            std::process::abort();
        }
    }
}

/// See [`LogSink`].
pub type LogSinkFn = Box<dyn Fn(ffi::LogLevel, &str) + Send + Sync>;

/// Holds the boxed log-sink closure installed through [`ffi::log_add_sink`].
///
/// May be invoked from arbitrary engine/spdlog threads, possibly concurrently, so the closure must
/// be `Send + Sync`; a panic inside it aborts the process rather than unwinding into C++.
pub struct LogSink(pub LogSinkFn);

fn log_sink_call(sink: &LogSink, level: ffi::LogLevel, message: &[u8]) {
    // spdlog messages are almost always UTF-8, but nothing guarantees it; go through a lossy
    // conversion (same approach as `input_text_this_tick` in the root shim) instead of letting a
    // stray byte turn into an abort at the FFI boundary.
    let message = String::from_utf8_lossy(message);
    guard("log sink", || (sink.0)(level, &message));
}

/// Holds the boxed one-shot closure consumed by [`ffi::scheduler_spawn`].
///
/// Runs once, on whichever worker thread the scheduler picks, so the closure must be `Send`; a
/// panic inside it aborts the process rather than unwinding into C++.
pub struct SchedulerTask(pub Option<Box<dyn FnOnce() + Send>>);

fn scheduler_task_run(task: &mut SchedulerTask) {
    if let Some(work) = task.0.take() {
        guard("scheduler task", work);
    }
}
