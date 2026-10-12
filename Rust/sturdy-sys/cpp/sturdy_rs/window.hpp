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
    HdrProperties window_hdr_properties(EngineView &engine);

} // namespace sturdy_rs::window
