//! Raw `cxx` bindings between Rust and the statically linked SturdyEngine 5 C++ runtime.
//!
//! This crate is the unsafe-ish plumbing; the ergonomic API lives in the `sturdy` crate. Nothing
//! here goes through the engine's C ABI: the shim in `cpp/sturdy_rs` calls the C++ API directly.
//!
//! One `#[cxx::bridge]` module per engine subsystem, each its own file — `build.rs` discovers and
//! compiles all of them automatically (see its module doc comment). Every bridge but this root one
//! reuses `EngineView` rather than redeclaring it: `type EngineView = crate::ffi::EngineView;`
//! under `#[namespace = "sturdy_rs"]` (required — see any of those modules' own comment on it, e.g.
//! `ecs::ffi`, for why the override matters).

pub mod app;
pub mod assets;
pub mod camera;
pub mod color;
pub mod device;
pub mod diagnostics;
pub mod ecs;
pub mod geometry;
pub mod hot_reload;
pub mod images;
pub mod reflection;
pub mod render;
pub mod rhi;
pub mod rhi_util;
pub mod schedule;
pub mod shader_compiler;
pub mod text;
pub mod text_render;
pub mod ui;
pub mod window;
pub mod ui_widgets;

#[cxx::bridge(namespace = "sturdy_rs")]
pub mod ffi {
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum VSync {
        Off,
        On,
        Adaptive,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum VariableRefresh {
        Disabled,
        Automatic,
        Preferred,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum LatencyMode {
        Normal,
        Low,
        Ultra,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum PresentationPreference {
        Automatic,
        LowestLatency,
        Smoothest,
        PowerEfficient,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum WindowMode {
        Windowed,
        BorderlessFullscreen,
        ExclusiveFullscreen,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum CameraProjection {
        Perspective,
        Orthographic,
        /// Uses `CameraDesc::custom_projection` as the projection matrix.
        Custom,
    }

    #[derive(Debug, Clone)]
    struct RuntimeOptions {
        window_title: String,
        app_name: String,
        /// Reverse-DNS application id (Wayland app_id / X11 WM_CLASS); empty = the window system's default.
        app_id: String,
        shaders_directory: String,
        width: u32,
        height: u32,
        resizable: bool,
        decorated: bool,
        high_dpi: bool,
        window_mode: WindowMode,
        raytracing: bool,
        vsync: VSync,
        variable_refresh: VariableRefresh,
        latency: LatencyMode,
        preference: PresentationPreference,
        /// Seconds between window-title refreshes; `<= 0` disables.
        title_update_interval_seconds: f64,
        runtime_window_management: bool,
        /// Vulkan physical device id (as reported by the GPU inventory) to force; empty lets the
        /// engine pick the best adapter.
        graphics_physical_device_id: String,
        /// A device request resolved against the GPU inventory before the device is created (see
        /// `EngineConfig::device_request`). `device_backend`: 0 Auto, 1 Vulkan, 2 D3D12, 3 Metal, 4 WebGPU.
        has_device_request: bool,
        device_backend: u8,
        device_adapter_id: String,
        /// Ordinal of `DevicePreference`.
        device_preference: u8,
        enable_shader_disk_cache: bool,
        prefer_async_compute: bool,
        /// 0 keeps the engine's default (2).
        desired_frames_in_flight: u32,
        /// Enables the native-access RHI extension that `Diagnostics::native_vulkan` etc. need.
        enable_native_access: bool,
        /// `RHI::feature_name` strings the device must support / should enable when available.
        required_features: Vec<String>,
        optional_features: Vec<String>,
        hdr_enabled: bool,
        /// Ordinal of `render::HdrColorSpaceMode` (0 = HDR10 ST.2084 .. 3 = Dolby Vision).
        hdr_color_space: u8,
        transparent_composition: bool,
        /// 0 lets the engine choose.
        swapchain_image_count: u32,
        allow_present_from_compute: bool,
        window_transparent: bool,
        window_visible: bool,
        has_window_position: bool,
        window_x: i32,
        window_y: i32,
    }

    #[derive(Debug, Clone)]
    struct Status {
        ok: bool,
        message: String,
    }

    #[derive(Debug, Clone, Copy)]
    struct FrameInfo {
        delta_seconds: f64,
        frame_index: u64,
        framebuffer_width: u32,
        framebuffer_height: u32,
        live_resize: bool,
        window_id: u64,
    }

    #[derive(Debug, Clone, Copy)]
    struct CameraDesc {
        projection: CameraProjection,
        position: [f32; 3],
        euler_degrees: [f32; 3],
        /// When true, `orientation` (xyzw quaternion) is used instead of `euler_degrees`.
        has_orientation: bool,
        orientation: [f32; 4],
        vertical_fov_degrees: f32,
        orthographic_size: f32,
        /// Column-major; used when `projection == Custom`.
        custom_projection: [f32; 16],
        aspect_ratio: f32,
        near_clip: f32,
        far_clip: f32,
        lens_shift: [f32; 2],
        jitter_ndc: [f32; 2],
        reverse_z: bool,
        /// When true, `focal_length_mm` is applied after the projection (and so overrides the FOV).
        has_focal_length: bool,
        focal_length_mm: f32,
        sensor_size_mm: [f32; 2],
        aperture_f_stop: f32,
        shutter_seconds: f32,
        iso: f32,
        focus_distance: f32,
        aperture_blades: u32,
        exposure_compensation_ev: f32,
        culling_mask: u32,
        priority: i32,
        active: bool,
        clear_color: [f32; 4],
        render_scale: f32,
        /// `x, y, width, height`, normalized.
        normalized_viewport: [f32; 4],
        /// Drop temporal history (previous view-projection) before this frame — a camera cut.
        reset_history: bool,
    }

    #[derive(Debug, Clone, Copy)]
    struct SceneLightingDesc {
        ambient_radiance: [f32; 3],
        exposure: f32,
    }

    #[derive(Debug, Clone)]
    struct FrameRequest {
        present: bool,
        /// Skip the 3D scene entirely: clear, draw the UI overlay, present (`RenderGraph::overlay_only`).
        overlay_only: bool,
        camera: CameraDesc,
        lighting: SceneLightingDesc,
        debug_label: String,
    }

    #[derive(Debug, Clone, Default)]
    struct GpuDescription {
        name: String,
        vendor: String,
        driver_version: String,
        api_version: String,
        device_type: String,
        vendor_id: u32,
        device_id: u32,
    }

    extern "Rust" {
        type LogicHandle;

        fn logic_on_init(logic: &mut LogicHandle, engine: Pin<&mut EngineView>) -> Status;
        fn logic_request_frame(
            logic: &mut LogicHandle,
            engine: Pin<&mut EngineView>,
            frame: &FrameInfo,
        ) -> FrameRequest;
        fn logic_on_shutdown(logic: &mut LogicHandle, engine: Pin<&mut EngineView>);
    }

    unsafe extern "C++" {
        include!("sturdy_rs/shim.hpp");

        /// `SFT::Engine::Engine`, only ever handled by reference.
        type EngineView;

        fn run_runtime(options: &RuntimeOptions, args: &Vec<String>, logic: Box<LogicHandle>) -> i32;

        fn engine_delta_seconds(engine: &EngineView) -> f64;
        fn engine_unscaled_delta_seconds(engine: &EngineView) -> f64;
        fn engine_tick_index(engine: &EngineView) -> u64;
        fn engine_time_scale(engine: &EngineView) -> f64;
        fn engine_set_time_scale(engine: Pin<&mut EngineView>, scale: f64);
        fn engine_gpu_description(engine: &EngineView, out: &mut GpuDescription) -> bool;

        fn input_key_down(engine: &EngineView, key: i32) -> bool;
        fn input_key_just_pressed(engine: &EngineView, key: i32) -> bool;
        fn input_key_just_released(engine: &EngineView, key: i32) -> bool;
        fn input_mouse_down(engine: &EngineView, button: u8) -> bool;
        fn input_mouse_just_pressed(engine: &EngineView, button: u8) -> bool;
        fn input_mouse_just_released(engine: &EngineView, button: u8) -> bool;
        fn input_mouse_position(engine: &EngineView) -> [f32; 2];
        fn input_mouse_delta(engine: &EngineView) -> [f32; 2];
        fn input_wheel_delta(engine: &EngineView) -> [f32; 2];
        fn input_text_this_tick(engine: &EngineView) -> String;
        /// `KeyModifiers` bitmask: Shift=1, Control=2, Alt=4, Super=8, CapsLock=16, NumLock=32.
        fn input_modifiers(engine: &EngineView) -> u32;
    }
}

use core::pin::Pin;

pub use ffi::EngineView;

/// See [`LogicHandle::on_init`].
pub type OnInitFn = Box<dyn FnMut(Pin<&mut EngineView>) -> ffi::Status + Send>;
/// See [`LogicHandle::request_frame`].
pub type RequestFrameFn = Box<dyn FnMut(Pin<&mut EngineView>, &ffi::FrameInfo) -> ffi::FrameRequest + Send>;
/// See [`LogicHandle::on_shutdown`].
pub type OnShutdownFn = Box<dyn FnMut(Pin<&mut EngineView>) + Send>;

/// Trampoline target for the C++ side: holds the user's boxed game logic. The `sturdy` crate
/// installs the vtable-like function table below so this crate stays free of user-facing traits.
pub struct LogicHandle {
    pub on_init: OnInitFn,
    pub request_frame: RequestFrameFn,
    pub on_shutdown: OnShutdownFn,
}

fn logic_on_init(logic: &mut LogicHandle, engine: Pin<&mut EngineView>) -> ffi::Status {
    (logic.on_init)(engine)
}

fn logic_request_frame(
    logic: &mut LogicHandle,
    engine: Pin<&mut EngineView>,
    frame: &ffi::FrameInfo,
) -> ffi::FrameRequest {
    (logic.request_frame)(engine, frame)
}

fn logic_on_shutdown(logic: &mut LogicHandle, engine: Pin<&mut EngineView>) {
    (logic.on_shutdown)(engine)
}
