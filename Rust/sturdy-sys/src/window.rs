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
        fn window_hdr_properties(engine: Pin<&mut EngineView>) -> HdrProperties;
    }
}
