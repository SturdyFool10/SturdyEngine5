//! Raw `cxx` bindings for direct control of the engine's **primary window** (`WindowManager::Window`
//! virtuals not reachable through the `WindowRequests` queue): title, size, position, limits,
//! minimize/maximize/restore/raise/show/hide/focus, opacity, cursor visibility, clipboard, HDR
//! properties, global cursor position.
//!
//! These call the window synchronously, so use them from the thread that owns the window system
//! (the game-logic thread). Secondary windows have no direct accessor in the engine (see the
//! engine plan); the queued `render::window_set_*` operations cover those.

#[cxx::bridge(namespace = "sturdy_rs::window")]
pub mod ffi {
    #[derive(Debug, Clone, Default)]
    struct WindowStatus {
        ok: bool,
        error: String,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct Int2Result {
        ok: bool,
        x: i32,
        y: i32,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct HdrProperties {
        /// `false` when the platform/window can't report HDR properties.
        available: bool,
        hdr_enabled: bool,
        sdr_white_level: f32,
        hdr_headroom: f32,
    }

    /// Mirrors `WindowManager::HitTestRegions`; rectangles are flat `x, y, w, h` quadruples.
    #[derive(Debug, Clone, Default)]
    struct HitTestRegionsInfo {
        enabled: bool,
        resize_border: u32,
        drag: Vec<i32>,
        no_drag: Vec<i32>,
    }

    #[derive(Debug, Clone, Default)]
    struct DisplayInfoBridge {
        id: u32,
        name: String,
        bounds: [i32; 4],
        usable_bounds: [i32; 4],
        content_scale: f32,
        refresh_rate_hz: f32,
        hdr_enabled: bool,
        primary: bool,
        /// `[width, height]` of the desktop mode, with its refresh rate and pixel density.
        current_mode: [u32; 2],
        current_refresh_hz: f32,
        current_pixel_density: f32,
        /// Every mode a fullscreen window may ask for: flat `width, height` pairs, parallel to `mode_refresh_hz`
        /// and `mode_pixel_density`.
        modes: Vec<u32>,
        mode_refresh_hz: Vec<f32>,
        mode_pixel_density: Vec<f32>,
        physical_size_mm: [u32; 2],
        /// 0 unknown, 1 landscape, 2 landscape flipped, 3 portrait, 4 portrait flipped.
        orientation: u8,
    }

    /// One `WindowFeature` answer; `note` explains an unsupported or partial feature.
    #[derive(Debug, Clone, Default)]
    struct FeatureInfo {
        supported: bool,
        note: String,
    }

    /// Frame, safe area and display of the primary window.
    #[derive(Debug, Clone, Copy, Default)]
    struct WindowGeometryInfo {
        /// left, top, right, bottom
        frame_extents: [i32; 4],
        /// x, y, width, height
        safe_area: [i32; 4],
        has_display: bool,
        display_id: u32,
        has_screen_keyboard: bool,
        screen_keyboard_shown: bool,
    }

    /// The display and mode a fullscreen window uses; unset parts mean "current display" / "desktop mode".
    #[derive(Debug, Clone, Copy, Default)]
    struct FullscreenTargetInfo {
        has_display: bool,
        display_id: u32,
        has_mode: bool,
        width: u32,
        height: u32,
        refresh_hz: f32,
        pixel_density: f32,
    }

    #[derive(Debug, Clone, Default)]
    struct ProfileResult {
        ok: bool,
        error: String,
        bytes: Vec<u8>,
    }

    /// One command for a window that is not the primary one, queued through the engine and run on the window thread.
    /// `op` is `Engine::WindowOperation`; the other fields are that operation's arguments (see `window.cpp`).
    #[derive(Debug, Clone, Default)]
    struct WindowCommand {
        op: u32,
        a: i64,
        b: i64,
        c: i64,
        d: i64,
        x: f32,
        y: f32,
        flag: bool,
        text: String,
        text2: String,
        bytes: Vec<u8>,
        durations: Vec<u32>,
        /// Flat `x, y, w, h` quadruples (hit-test drag areas, and the clickable areas inside them).
        rects: Vec<i32>,
        no_drag_rects: Vec<i32>,
        /// A second window (parenting).
        other_window: u64,
        has_other: bool,
    }

    /// The last outcome of one operation on one window, with a query's output.
    #[derive(Debug, Clone, Default)]
    struct OperationResult {
        completed: bool,
        ok: bool,
        /// 0 none, 1 unsupported, 2 invalid argument, 3 failed
        error_kind: u8,
        message: String,
        flag: bool,
        /// The button a message box returned.
        number: i32,
        text: String,
        bytes: Vec<u8>,
        list: Vec<String>,
    }

    #[derive(Debug, Clone, Default)]
    struct GamepadInfoBridge {
        id: u32,
        name: String,
        /// `Gamepads::DeviceKind` ordinal.
        kind: u8,
        vendor: u16,
        product: u16,
        /// 0 unknown, 1 wired, 2 wireless
        connection: u8,
        player_index: i32,
        mapped: bool,
        has_rumble: bool,
        has_trigger_rumble: bool,
        has_led: bool,
        has_accelerometer: bool,
        has_gyroscope: bool,
        has_touchpad: bool,
        battery_percent: i32,
        charging: bool,
    }

    #[derive(Debug, Clone, Default)]
    struct GamepadStateBridge {
        available: bool,
        buttons: u32,
        axes: [f32; 6],
    }

    #[derive(Debug, Clone, Default)]
    struct GamepadRawBridge {
        available: bool,
        axes: Vec<f32>,
        buttons: Vec<u8>,
        hats: Vec<u8>,
    }

    /// `kind` 0 connected, 1 disconnected.
    #[derive(Debug, Clone, Copy, Default)]
    struct GamepadEventBridge {
        kind: u8,
        id: u32,
    }

    /// One finger from the engine's per-tick input state.
    #[derive(Debug, Clone, Copy, Default)]
    struct TouchPointInfo {
        window: u64,
        device: u64,
        finger: u64,
        x: f32,
        y: f32,
        dx: f32,
        dy: f32,
        pressure: f32,
        down: bool,
        just_pressed: bool,
        just_released: bool,
        cancelled: bool,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct PenPointInfo {
        window: u64,
        pen: u32,
        x: f32,
        y: f32,
        in_range: bool,
        down: bool,
        eraser: bool,
        just_down: bool,
        just_up: bool,
        pressure: f32,
        x_tilt: f32,
        y_tilt: f32,
        distance: f32,
        rotation: f32,
        slider: f32,
        tangential_pressure: f32,
        buttons: u32,
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct GamepadFrameInfo {
        id: u32,
        buttons: u32,
        pressed_this_tick: u32,
        released_this_tick: u32,
        axes: [f32; 6],
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct SensorReading {
        ok: bool,
        xyz: [f32; 3],
    }

    #[derive(Debug, Clone, Default)]
    struct MessageBoxResult {
        ok: bool,
        error: String,
        button: i32,
    }

    /// One dialog outcome; `ready` is false until the user has closed it.
    #[derive(Debug, Clone, Default)]
    struct FileDialogOutcome {
        ready: bool,
        cancelled: bool,
        failed: bool,
        error: String,
        paths: Vec<String>,
        filter_index: i32,
    }

    /// One tray menu row; `parent` is the index of the submenu row it belongs to, or -1 for the top level.
    #[derive(Debug, Clone, Default)]
    struct TrayEntryInfo {
        id: u32,
        label: String,
        separator: bool,
        checkable: bool,
        checked: bool,
        enabled: bool,
        parent: i32,
    }

    #[derive(Debug, Clone, Default)]
    struct TrayResult {
        ok: bool,
        error: String,
        handle: u64,
    }

    /// `Engine::DropEvent`; `kind` is 0 begin, 1 hover, 2 file, 3 text, 4 complete.
    #[derive(Debug, Clone, Default)]
    struct DropEventInfo {
        window_id: u64,
        kind: u8,
        x: i32,
        y: i32,
        data: String,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/window.hpp");

        #[namespace = "sturdy_rs"]
        type EngineView = crate::ffi::EngineView;

        fn window_has_primary(engine: Pin<&mut EngineView>) -> bool;
        fn window_close_requested(engine: Pin<&mut EngineView>) -> bool;
        fn window_request_close(engine: Pin<&mut EngineView>);
        fn window_show(engine: Pin<&mut EngineView>) -> WindowStatus;
        fn window_hide(engine: Pin<&mut EngineView>) -> WindowStatus;
        fn window_focus(engine: Pin<&mut EngineView>) -> WindowStatus;
        fn window_raise(engine: Pin<&mut EngineView>) -> WindowStatus;
        fn window_maximize(engine: Pin<&mut EngineView>) -> WindowStatus;
        fn window_minimize(engine: Pin<&mut EngineView>) -> WindowStatus;
        fn window_restore(engine: Pin<&mut EngineView>) -> WindowStatus;
        fn window_set_title(engine: Pin<&mut EngineView>, title: &str) -> WindowStatus;
        fn window_position(engine: Pin<&mut EngineView>) -> Int2Result;
        fn window_set_position(engine: Pin<&mut EngineView>, x: i32, y: i32) -> WindowStatus;
        fn window_global_cursor_position(engine: Pin<&mut EngineView>) -> Int2Result;
        fn window_size(engine: Pin<&mut EngineView>) -> Int2Result;
        fn window_set_size(engine: Pin<&mut EngineView>, width: u32, height: u32) -> WindowStatus;
        fn window_set_minimum_size(engine: Pin<&mut EngineView>, width: u32, height: u32) -> WindowStatus;
        fn window_set_maximum_size(engine: Pin<&mut EngineView>, width: u32, height: u32) -> WindowStatus;
        fn window_set_resizable(engine: Pin<&mut EngineView>, resizable: bool) -> WindowStatus;
        /// Ordinal of `render::WindowMode` (windowed, borderless, exclusive, ...).
        fn window_fullscreen_mode(engine: Pin<&mut EngineView>) -> u8;
        fn window_set_opacity(engine: Pin<&mut EngineView>, opacity: f32) -> WindowStatus;
        fn window_opacity(engine: Pin<&mut EngineView>, out: &mut f32) -> WindowStatus;
        fn window_set_cursor_visible(engine: Pin<&mut EngineView>, visible: bool) -> WindowStatus;
        fn window_clipboard_text(engine: Pin<&mut EngineView>) -> String;
        fn window_set_clipboard_text(engine: Pin<&mut EngineView>, text: &str) -> WindowStatus;
        /// Typed clipboard payloads ("image/png", "text/uri-list", ...). Unsupported providers report none.
        fn window_clipboard_has_mime(engine: Pin<&mut EngineView>, mime: &str) -> bool;
        fn window_clipboard_data(engine: Pin<&mut EngineView>, mime: &str) -> Vec<u8>;
        fn window_set_clipboard_data(engine: Pin<&mut EngineView>, mime: &str, bytes: &[u8]) -> WindowStatus;
        fn window_hdr_properties(engine: Pin<&mut EngineView>) -> HdrProperties;

        // -- Custom title bar / chrome --
        fn window_set_hit_test_regions(engine: Pin<&mut EngineView>, regions: &HitTestRegionsInfo) -> WindowStatus;
        fn window_show_system_menu(engine: Pin<&mut EngineView>, x: i32, y: i32) -> WindowStatus;
        fn window_is_maximized(engine: Pin<&mut EngineView>) -> bool;
        fn window_is_minimized(engine: Pin<&mut EngineView>) -> bool;
        fn window_set_icon(engine: Pin<&mut EngineView>, width: u32, height: u32, rgba: &[u8]) -> WindowStatus;
        /// 0 cancel, 1 briefly, 2 until focused.
        fn window_request_attention(engine: Pin<&mut EngineView>, request: u8) -> WindowStatus;
        /// 0 none, 1 indeterminate, 2 normal, 3 paused, 4 error.
        fn window_set_taskbar_progress(engine: Pin<&mut EngineView>, state: u8, value: f32) -> WindowStatus;
        /// 0 unknown, 1 light, 2 dark.
        fn window_system_theme(engine: Pin<&mut EngineView>) -> u8;
        fn window_displays(engine: Pin<&mut EngineView>) -> Vec<DisplayInfoBridge>;
        fn take_drop_events(engine: Pin<&mut EngineView>) -> Vec<DropEventInfo>;

        // -- The union surface: what the window provider can do, and the behaviour built on it --
        /// `WindowFeature` ordinal -> its answer.
        fn window_feature(engine: Pin<&mut EngineView>, feature: u32) -> FeatureInfo;
        fn window_geometry(engine: Pin<&mut EngineView>) -> WindowGeometryInfo;
        fn window_set_always_on_top(engine: Pin<&mut EngineView>, enabled: bool) -> WindowStatus;
        fn window_set_aspect_ratio(engine: Pin<&mut EngineView>, min_ratio: f32, max_ratio: f32) -> WindowStatus;
        fn window_set_mouse_passthrough(engine: Pin<&mut EngineView>, enabled: bool) -> WindowStatus;
        fn window_set_focusable(engine: Pin<&mut EngineView>, enabled: bool) -> WindowStatus;
        fn window_set_auto_minimize_on_focus_loss(engine: Pin<&mut EngineView>, enabled: bool) -> WindowStatus;
        /// `parent_window` is another window's id (`u64::MAX` detaches).
        fn window_set_parent(engine: Pin<&mut EngineView>, parent_window: u64, modal: bool) -> WindowStatus;
        fn window_set_keyboard_grab(engine: Pin<&mut EngineView>, enabled: bool) -> WindowStatus;
        fn window_set_mouse_confine_rect(engine: Pin<&mut EngineView>, has_rect: bool, x: i32, y: i32, width: i32, height: i32) -> WindowStatus;
        fn window_set_cursor_position(engine: Pin<&mut EngineView>, x: i32, y: i32) -> WindowStatus;
        fn window_set_global_cursor_position(engine: Pin<&mut EngineView>, x: i32, y: i32) -> WindowStatus;
        fn window_set_mouse_capture(engine: Pin<&mut EngineView>, enabled: bool) -> WindowStatus;
        fn window_set_custom_cursor(engine: Pin<&mut EngineView>, width: u32, height: u32, rgba: &[u8], hot_x: i32, hot_y: i32) -> WindowStatus;
        /// `frames` is every frame's RGBA8 pixels back to back; `durations_ms` has one entry per frame.
        fn window_set_animated_cursor(
            engine: Pin<&mut EngineView>,
            width: u32,
            height: u32,
            frames: &[u8],
            durations_ms: &[u32],
            hot_x: i32,
            hot_y: i32,
        ) -> WindowStatus;
        fn window_clear_custom_cursor(engine: Pin<&mut EngineView>) -> WindowStatus;
        /// An empty `coverage` restores the rectangle.
        fn window_set_shape(engine: Pin<&mut EngineView>, width: u32, height: u32, coverage: &[u8]) -> WindowStatus;
        /// `WindowType` ordinal.
        fn window_set_window_type(engine: Pin<&mut EngineView>, window_type: u8) -> WindowStatus;
        fn window_set_display_gamma(engine: Pin<&mut EngineView>, has_display: bool, display_id: u32, gamma: f32) -> WindowStatus;
        fn window_set_fullscreen_target(engine: Pin<&mut EngineView>, target: &FullscreenTargetInfo) -> WindowStatus;
        fn window_start_text_input_with(engine: Pin<&mut EngineView>, input_type: u8, capitalization: u8, autocorrect: bool, multiline: bool)
            -> WindowStatus;
        fn window_clipboard_mime_types(engine: Pin<&mut EngineView>) -> Vec<String>;
        fn window_key_name(engine: Pin<&mut EngineView>, key: i32) -> String;
        fn window_key_down(engine: Pin<&mut EngineView>, key: i32) -> bool;
        fn window_icc_profile(engine: Pin<&mut EngineView>) -> ProfileResult;

        /// How many `WindowFeature` / `WindowOperation` values the engine has, so the Rust enums can be checked against it.
        fn window_feature_count() -> u32;
        fn window_operation_count() -> u32;

        // -- Any window, queued (secondary windows have no synchronous accessor) --
        fn window_run_command(engine: Pin<&mut EngineView>, window_id: u64, command: &WindowCommand) -> WindowStatus;
        fn window_operation_result(engine: Pin<&mut EngineView>, window_id: u64, op: u32) -> OperationResult;
        /// A window's capability answer from the engine snapshot, for any window.
        fn window_feature_of(engine: Pin<&mut EngineView>, window_id: u64, feature: u32) -> FeatureInfo;

        // -- Per-tick input state: touch, pen and (once enabled) gamepads --
        fn input_touches(engine: &EngineView) -> Vec<TouchPointInfo>;
        fn input_pens(engine: &EngineView) -> Vec<PenPointInfo>;
        fn input_set_gamepads_enabled(engine: Pin<&mut EngineView>, enabled: bool);
        fn input_gamepads(engine: &EngineView) -> Vec<GamepadFrameInfo>;
        fn input_gamepad_events(engine: &EngineView) -> Vec<GamepadEventBridge>;

        // -- Gamepads and joysticks, whatever the window provider --
        fn gamepad_list(error: &mut String) -> Vec<GamepadInfoBridge>;
        fn gamepad_state(id: u32) -> GamepadStateBridge;
        fn gamepad_raw_state(id: u32) -> GamepadRawBridge;
        fn gamepad_take_events() -> Vec<GamepadEventBridge>;
        fn gamepad_rumble(id: u32, low: f32, high: f32, duration_ms: u32) -> WindowStatus;
        fn gamepad_rumble_triggers(id: u32, left: f32, right: f32, duration_ms: u32) -> WindowStatus;
        fn gamepad_set_led(id: u32, red: u8, green: u8, blue: u8) -> WindowStatus;
        fn gamepad_set_player_index(id: u32, player_index: i32) -> WindowStatus;
        /// `sensor` 0 accelerometer, 1 gyroscope.
        fn gamepad_set_sensor_enabled(id: u32, sensor: u8, enabled: bool) -> WindowStatus;
        fn gamepad_sensor(id: u32, sensor: u8) -> SensorReading;

        // -- Desktop services, whatever the window provider --
        fn desktop_set_screensaver_enabled(enabled: bool) -> WindowStatus;
        fn desktop_screensaver_enabled(out: &mut bool) -> WindowStatus;
        fn desktop_message_box(kind: u8, title: &str, message: &str, buttons: &[String], default_button: i32, escape_button: i32) -> MessageBoxResult;
        /// `kind` 0 open file, 1 save file, 2 open folder; filters are parallel `names` / `patterns`. Returns a
        /// ticket for `desktop_file_dialog_take`, or 0 when the dialog could not be started (`error` says why).
        fn desktop_file_dialog_start(
            kind: u8,
            title: &str,
            accept_label: &str,
            cancel_label: &str,
            default_location: &str,
            filter_names: &[String],
            filter_patterns: &[String],
            allow_multiple: bool,
            error: &mut String,
        ) -> u64;
        fn desktop_file_dialog_take(ticket: u64) -> FileDialogOutcome;
        /// `desktop_file_dialog_start`, parented to a window. The dialog opens on that window's thread.
        fn desktop_file_dialog_start_for(
            engine: Pin<&mut EngineView>,
            window_id: u64,
            kind: u8,
            title: &str,
            accept_label: &str,
            cancel_label: &str,
            default_location: &str,
            filter_names: &[String],
            filter_patterns: &[String],
            allow_multiple: bool,
            error: &mut String,
        ) -> u64;
        /// A message box over a window, queued; the pressed button is `OperationResult::number` of operation 47.
        fn window_show_message_box(
            engine: Pin<&mut EngineView>,
            window_id: u64,
            kind: u8,
            title: &str,
            message: &str,
            buttons: &[String],
            default_button: i32,
            escape_button: i32,
        ) -> WindowStatus;
        fn desktop_pump_events();
        fn tray_create(width: u32, height: u32, rgba: &[u8], tooltip: &str, entries: &[TrayEntryInfo]) -> TrayResult;
        fn tray_set_icon(handle: u64, width: u32, height: u32, rgba: &[u8]) -> WindowStatus;
        fn tray_set_tooltip(handle: u64, tooltip: &str) -> WindowStatus;
        fn tray_set_menu(handle: u64, entries: &[TrayEntryInfo]) -> WindowStatus;
        /// The ids of the menu rows clicked since the last call.
        fn tray_take_clicks(handle: u64) -> Vec<u32>;
        fn tray_destroy(handle: u64) -> WindowStatus;
    }
}
