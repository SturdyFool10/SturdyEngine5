// Rust <-> SturdyEngine 5 shim for direct primary-window control. See src/window.rs.
#pragma once

#include <cstdint>

#include <rust/cxx.h>

#include "sturdy_rs/shim.hpp"

namespace sturdy_rs::window {

    using EngineView = sturdy_rs::EngineView;

    struct WindowStatus;
    struct Int2Result;
    struct HdrProperties;
    struct HitTestRegionsInfo;
    struct DisplayInfoBridge;
    struct DropEventInfo;

    WindowStatus window_set_hit_test_regions(EngineView &engine, const HitTestRegionsInfo &regions);
    WindowStatus window_show_system_menu(EngineView &engine, std::int32_t x, std::int32_t y);
    bool window_is_maximized(EngineView &engine) noexcept;
    bool window_is_minimized(EngineView &engine) noexcept;
    WindowStatus window_set_icon(EngineView &engine, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba);
    WindowStatus window_request_attention(EngineView &engine, std::uint8_t request);
    WindowStatus window_set_taskbar_progress(EngineView &engine, std::uint8_t state, float value);
    std::uint8_t window_system_theme(EngineView &engine) noexcept;
    rust::Vec<DisplayInfoBridge> window_displays(EngineView &engine);
    rust::Vec<DropEventInfo> take_drop_events(EngineView &engine);

    bool window_has_primary(EngineView &engine) noexcept;
    bool window_close_requested(EngineView &engine) noexcept;
    void window_request_close(EngineView &engine) noexcept;
    WindowStatus window_show(EngineView &engine);
    WindowStatus window_hide(EngineView &engine);
    WindowStatus window_focus(EngineView &engine);
    WindowStatus window_raise(EngineView &engine);
    WindowStatus window_maximize(EngineView &engine);
    WindowStatus window_minimize(EngineView &engine);
    WindowStatus window_restore(EngineView &engine);
    WindowStatus window_set_title(EngineView &engine, rust::Str title);
    Int2Result window_position(EngineView &engine);
    WindowStatus window_set_position(EngineView &engine, std::int32_t x, std::int32_t y);
    Int2Result window_global_cursor_position(EngineView &engine);
    Int2Result window_size(EngineView &engine);
    WindowStatus window_set_size(EngineView &engine, std::uint32_t width, std::uint32_t height);
    WindowStatus window_set_minimum_size(EngineView &engine, std::uint32_t width, std::uint32_t height);
    WindowStatus window_set_maximum_size(EngineView &engine, std::uint32_t width, std::uint32_t height);
    WindowStatus window_set_resizable(EngineView &engine, bool resizable);
    std::uint8_t window_fullscreen_mode(EngineView &engine) noexcept;
    WindowStatus window_set_opacity(EngineView &engine, float opacity);
    WindowStatus window_opacity(EngineView &engine, float &out);
    WindowStatus window_set_cursor_visible(EngineView &engine, bool visible);
    rust::String window_clipboard_text(EngineView &engine);
    WindowStatus window_set_clipboard_text(EngineView &engine, rust::Str text);
    bool window_clipboard_has_mime(EngineView &engine, rust::Str mime);
    rust::Vec<std::uint8_t> window_clipboard_data(EngineView &engine, rust::Str mime);
    WindowStatus window_set_clipboard_data(EngineView &engine, rust::Str mime, rust::Slice<const std::uint8_t> bytes);
    HdrProperties window_hdr_properties(EngineView &engine);

} // namespace sturdy_rs::window
