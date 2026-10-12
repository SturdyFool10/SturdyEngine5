#include "sturdy_rs/window.hpp"

#include <string>

#include <Engine/Engine.hpp>
#include <WindowManager/WindowManager.hpp>

// Generated from src/window.rs.
#include "sturdy_rs/window_bridge.h"

namespace sturdy_rs::window {
    namespace {

        namespace W = SFT::WindowManager;

        W::Window *primary(EngineView &engine) noexcept { return engine.primary_window(); }

        WindowStatus no_window() { return WindowStatus{false, rust::String("the engine has no primary window")}; }

        WindowStatus status(const std::expected<void, W::WindowError> &result) {
            if (result) {
                return WindowStatus{true, rust::String()};
            }
            return WindowStatus{false, rust::String::lossy(result.error().message)};
        }

        template <class F>
        WindowStatus with_window(EngineView &engine, F &&call) {
            W::Window *window = primary(engine);
            if (window == nullptr) {
                return no_window();
            }
            return status(call(*window));
        }

        Int2Result int2(const std::expected<W::WindowExtent, W::WindowError> &r) {
            return r ? Int2Result{true, static_cast<std::int32_t>(r->x), static_cast<std::int32_t>(r->y)} : Int2Result{};
        }

        Int2Result int2(const std::expected<W::WindowPosition, W::WindowError> &r) {
            return r ? Int2Result{true, r->x, r->y} : Int2Result{};
        }

    } // namespace

    bool window_has_primary(EngineView &e) noexcept { return primary(e) != nullptr; }
    bool window_close_requested(EngineView &e) noexcept {
        W::Window *w = primary(e);
        return w != nullptr && w->close_requested();
    }
    void window_request_close(EngineView &e) noexcept {
        if (W::Window *w = primary(e)) {
            w->request_close();
        }
    }
    WindowStatus window_show(EngineView &e) { return with_window(e, [](W::Window &w) { return w.show(); }); }
    WindowStatus window_hide(EngineView &e) { return with_window(e, [](W::Window &w) { return w.hide(); }); }
    WindowStatus window_focus(EngineView &e) { return with_window(e, [](W::Window &w) { return w.focus(); }); }
    WindowStatus window_raise(EngineView &e) { return with_window(e, [](W::Window &w) { return w.raise(); }); }
    WindowStatus window_maximize(EngineView &e) { return with_window(e, [](W::Window &w) { return w.maximize(); }); }
    WindowStatus window_minimize(EngineView &e) { return with_window(e, [](W::Window &w) { return w.minimize(); }); }
    WindowStatus window_restore(EngineView &e) { return with_window(e, [](W::Window &w) { return w.restore(); }); }
    WindowStatus window_set_title(EngineView &e, rust::Str title) {
        const std::string owned{title.data(), title.size()};
        return with_window(e, [&](W::Window &w) { return w.set_title(owned.c_str()); });
    }
    Int2Result window_position(EngineView &e) {
        W::Window *w = primary(e);
        return w ? int2(w->position()) : Int2Result{};
    }
    WindowStatus window_set_position(EngineView &e, std::int32_t x, std::int32_t y) {
        return with_window(e, [&](W::Window &w) { return w.set_position(W::WindowPosition{x, y}); });
    }
    Int2Result window_global_cursor_position(EngineView &e) {
        W::Window *w = primary(e);
        return w ? int2(w->global_cursor_position()) : Int2Result{};
    }
    Int2Result window_size(EngineView &e) {
        W::Window *w = primary(e);
        return w ? int2(w->size()) : Int2Result{};
    }
    WindowStatus window_set_size(EngineView &e, std::uint32_t width, std::uint32_t height) {
        return with_window(e, [&](W::Window &w) { return w.set_size(W::WindowExtent{width, height}); });
    }
    WindowStatus window_set_minimum_size(EngineView &e, std::uint32_t width, std::uint32_t height) {
        return with_window(e, [&](W::Window &w) { return w.set_minimum_size(W::WindowExtent{width, height}); });
    }
    WindowStatus window_set_maximum_size(EngineView &e, std::uint32_t width, std::uint32_t height) {
        return with_window(e, [&](W::Window &w) { return w.set_maximum_size(W::WindowExtent{width, height}); });
    }
    WindowStatus window_set_resizable(EngineView &e, bool resizable) {
        return with_window(e, [&](W::Window &w) { return w.set_resizable(resizable); });
    }
    std::uint8_t window_fullscreen_mode(EngineView &e) noexcept {
        W::Window *w = primary(e);
        return w ? static_cast<std::uint8_t>(w->fullscreen_mode()) : 0;
    }
    WindowStatus window_set_opacity(EngineView &e, float opacity) {
        return with_window(e, [&](W::Window &w) { return w.set_opacity(opacity); });
    }
    WindowStatus window_opacity(EngineView &e, float &out) {
        W::Window *w = primary(e);
        if (w == nullptr) {
            return no_window();
        }
        auto value = w->opacity();
        if (!value) {
            return WindowStatus{false, rust::String::lossy(value.error().message)};
        }
        out = *value;
        return WindowStatus{true, rust::String()};
    }
    WindowStatus window_set_cursor_visible(EngineView &e, bool visible) {
        return with_window(e, [&](W::Window &w) { return w.set_cursor_visible(visible); });
    }
    rust::String window_clipboard_text(EngineView &e) {
        W::Window *w = primary(e);
        return w ? rust::String::lossy(w->clipboard_text()) : rust::String();
    }
    WindowStatus window_set_clipboard_text(EngineView &e, rust::Str text) {
        return with_window(e, [&](W::Window &w) { return w.set_clipboard_text(std::string_view{text.data(), text.size()}); });
    }
    HdrProperties window_hdr_properties(EngineView &e) {
        HdrProperties out{};
        if (W::Window *w = primary(e)) {
            if (const auto props = w->hdr_properties()) {
                out = HdrProperties{true, props->hdr_enabled, props->sdr_white_level, props->hdr_headroom};
            }
        }
        return out;
    }

} // namespace sturdy_rs::window
