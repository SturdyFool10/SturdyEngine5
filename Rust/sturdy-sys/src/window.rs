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
    }
}
