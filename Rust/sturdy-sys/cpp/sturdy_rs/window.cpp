#include "sturdy_rs/window.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <Engine/Engine.hpp>
#include <Engine/WindowControl.hpp>
#include <WindowManager/Desktop.hpp>
#include <WindowManager/Gamepad.hpp>
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
    bool window_clipboard_has_mime(EngineView &e, rust::Str mime) {
        W::Window *w = primary(e);
        return w != nullptr && w->clipboard_has_mime(std::string_view{mime.data(), mime.size()});
    }
    rust::Vec<std::uint8_t> window_clipboard_data(EngineView &e, rust::Str mime) {
        rust::Vec<std::uint8_t> out;
        if (W::Window *w = primary(e)) {
            for (const std::byte b : w->clipboard_data(std::string_view{mime.data(), mime.size()})) {
                out.push_back(static_cast<std::uint8_t>(b));
            }
        }
        return out;
    }
    WindowStatus window_set_clipboard_data(EngineView &e, rust::Str mime, rust::Slice<const std::uint8_t> bytes) {
        return with_window(e, [&](W::Window &w) {
            return w.set_clipboard_data(std::string_view{mime.data(), mime.size()},
                                        std::span<const std::byte>{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()});
        });
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

    namespace {
        std::vector<W::WindowRect> rects_from(const rust::Vec<std::int32_t> &flat) {
            std::vector<W::WindowRect> out;
            for (std::size_t i = 0; i + 3 < flat.size(); i += 4) {
                out.push_back(W::WindowRect{flat[i], flat[i + 1], flat[i + 2], flat[i + 3]});
            }
            return out;
        }
    } // namespace

    WindowStatus window_set_hit_test_regions(EngineView &e, const HitTestRegionsInfo &regions) {
        W::HitTestRegions native;
        native.enabled = regions.enabled;
        native.resize_border = regions.resize_border;
        native.drag = rects_from(regions.drag);
        native.no_drag = rects_from(regions.no_drag);
        return with_window(e, [&](W::Window &w) { return w.set_hit_test_regions(std::move(native)); });
    }

    WindowStatus window_show_system_menu(EngineView &e, std::int32_t x, std::int32_t y) {
        return with_window(e, [&](W::Window &w) { return w.show_system_menu(W::WindowPosition{x, y}); });
    }

    bool window_is_maximized(EngineView &e) noexcept {
        W::Window *w = primary(e);
        return w != nullptr && w->maximized();
    }

    bool window_is_minimized(EngineView &e) noexcept {
        W::Window *w = primary(e);
        return w != nullptr && w->minimized();
    }

    WindowStatus window_set_icon(EngineView &e, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba) {
        const std::span<const std::byte> bytes{reinterpret_cast<const std::byte *>(rgba.data()), rgba.size()};
        return with_window(e, [&](W::Window &w) { return w.set_icon(width, height, bytes); });
    }

    WindowStatus window_request_attention(EngineView &e, std::uint8_t request) {
        const W::AttentionRequest native = request == 0   ? W::AttentionRequest::Cancel
                                           : request == 1 ? W::AttentionRequest::Briefly
                                                          : W::AttentionRequest::UntilFocused;
        return with_window(e, [&](W::Window &w) { return w.request_attention(native); });
    }

    WindowStatus window_set_taskbar_progress(EngineView &e, std::uint8_t state, float value) {
        const auto native = static_cast<W::TaskbarProgress>(std::min<std::uint8_t>(state, 4));
        return with_window(e, [&](W::Window &w) { return w.set_taskbar_progress(native, value); });
    }

    std::uint8_t window_system_theme(EngineView &e) noexcept {
        W::Window *w = primary(e);
        return w != nullptr ? static_cast<std::uint8_t>(w->system_theme()) : 0;
    }

    rust::Vec<DisplayInfoBridge> window_displays(EngineView &e) {
        rust::Vec<DisplayInfoBridge> out;
        W::Window *w = primary(e);
        if (w == nullptr) {
            return out;
        }
        for (const W::DisplayInfo &display : w->displays()) {
            DisplayInfoBridge info{};
            info.id = display.id;
            info.name = rust::String::lossy(display.name);
            info.bounds = {display.bounds.x, display.bounds.y, display.bounds.width, display.bounds.height};
            info.usable_bounds = {display.usable_bounds.x, display.usable_bounds.y, display.usable_bounds.width,
                                  display.usable_bounds.height};
            info.content_scale = display.content_scale;
            info.refresh_rate_hz = display.refresh_rate_hz;
            info.hdr_enabled = display.hdr_enabled;
            info.primary = display.primary;
            info.current_mode = {display.current_mode.width, display.current_mode.height};
            info.current_refresh_hz = display.current_mode.refresh_rate_hz;
            info.current_pixel_density = display.current_mode.pixel_density;
            for (const W::DisplayMode &mode : display.modes) {
                info.modes.push_back(mode.width);
                info.modes.push_back(mode.height);
                info.mode_refresh_hz.push_back(mode.refresh_rate_hz);
                info.mode_pixel_density.push_back(mode.pixel_density);
            }
            info.physical_size_mm = {display.physical_width_mm, display.physical_height_mm};
            info.orientation = static_cast<std::uint8_t>(display.orientation);
            out.push_back(std::move(info));
        }
        return out;
    }

    // -- The union surface ----------------------------------------------------------------------------------------------

    namespace {

        namespace E = SFT::Engine;

        FeatureInfo feature_info(const W::FeatureSupport &support) {
            return FeatureInfo{support.supported, rust::String::lossy(std::string{support.note})};
        }

        std::span<const std::byte> as_bytes(rust::Slice<const std::uint8_t> slice) {
            return {reinterpret_cast<const std::byte *>(slice.data()), slice.size()};
        }

        std::vector<std::byte> copy_bytes(rust::Slice<const std::uint8_t> slice) {
            const auto bytes = as_bytes(slice);
            return std::vector<std::byte>(bytes.begin(), bytes.end());
        }

        std::vector<std::byte> copy_bytes(const rust::Vec<std::uint8_t> &vec) {
            const auto *bytes = reinterpret_cast<const std::byte *>(vec.data());
            return std::vector<std::byte>(bytes, bytes + vec.size());
        }

        W::HitTestRegions hit_regions(bool enabled, std::uint32_t border, const rust::Vec<std::int32_t> &drag, const rust::Vec<std::int32_t> &no_drag) {
            W::HitTestRegions regions;
            regions.enabled = enabled;
            regions.resize_border = border;
            for (std::size_t i = 0; i + 3 < drag.size(); i += 4) {
                regions.drag.push_back(W::WindowRect{drag[i], drag[i + 1], drag[i + 2], drag[i + 3]});
            }
            for (std::size_t i = 0; i + 3 < no_drag.size(); i += 4) {
                regions.no_drag.push_back(W::WindowRect{no_drag[i], no_drag[i + 1], no_drag[i + 2], no_drag[i + 3]});
            }
            return regions;
        }

        W::FullscreenTarget to_target(const FullscreenTargetInfo &info) {
            W::FullscreenTarget target;
            if (info.has_display) {
                target.display_id = info.display_id;
            }
            if (info.has_mode) {
                target.mode = W::DisplayMode{info.width, info.height, info.refresh_hz, info.pixel_density};
            }
            return target;
        }

        /// A window's native handle from the engine snapshot, which is how windows are related across providers.
        std::optional<W::NativeWindowHandle> native_of(EngineView &e, std::uint64_t window_id) {
            const E::WindowSnapshot *snapshot = e.window_state().find(static_cast<W::WindowId>(window_id));
            return snapshot != nullptr ? snapshot->native_handle : std::nullopt;
        }

    } // namespace

    FeatureInfo window_feature(EngineView &e, std::uint32_t feature) {
        W::Window *w = primary(e);
        if (w == nullptr || feature >= static_cast<std::uint32_t>(W::WindowFeature::Count)) {
            return FeatureInfo{false, rust::String("no such window or feature")};
        }
        return feature_info(w->feature_support(static_cast<W::WindowFeature>(feature)));
    }

    FeatureInfo window_feature_of(EngineView &e, std::uint64_t window_id, std::uint32_t feature) {
        const E::WindowSnapshot *snapshot = e.window_state().find(static_cast<W::WindowId>(window_id));
        if (snapshot == nullptr || feature >= static_cast<std::uint32_t>(W::WindowFeature::Count)) {
            return FeatureInfo{false, rust::String("no such window or feature")};
        }
        return feature_info(snapshot->feature(static_cast<W::WindowFeature>(feature)));
    }

    WindowGeometryInfo window_geometry(EngineView &e) {
        WindowGeometryInfo info{};
        W::Window *w = primary(e);
        if (w == nullptr) {
            return info;
        }
        if (const auto frame = w->frame_extents()) {
            info.frame_extents = {frame->left, frame->top, frame->right, frame->bottom};
        }
        if (const auto area = w->safe_area()) {
            info.safe_area = {area->x, area->y, area->width, area->height};
        }
        if (const auto display = w->current_display_id()) {
            info.has_display = true;
            info.display_id = *display;
        }
        info.has_screen_keyboard = w->has_screen_keyboard();
        info.screen_keyboard_shown = w->screen_keyboard_shown();
        return info;
    }

    WindowStatus window_set_always_on_top(EngineView &e, bool enabled) {
        return with_window(e, [&](W::Window &w) { return w.set_always_on_top(enabled); });
    }
    WindowStatus window_set_aspect_ratio(EngineView &e, float min_ratio, float max_ratio) {
        return with_window(e, [&](W::Window &w) { return w.set_aspect_ratio(min_ratio, max_ratio); });
    }
    WindowStatus window_set_mouse_passthrough(EngineView &e, bool enabled) {
        return with_window(e, [&](W::Window &w) { return w.set_mouse_passthrough(enabled); });
    }
    WindowStatus window_set_focusable(EngineView &e, bool enabled) {
        return with_window(e, [&](W::Window &w) { return w.set_focusable(enabled); });
    }
    WindowStatus window_set_auto_minimize_on_focus_loss(EngineView &e, bool enabled) {
        return with_window(e, [&](W::Window &w) { return w.set_auto_minimize_on_focus_loss(enabled); });
    }
    WindowStatus window_set_parent(EngineView &e, std::uint64_t parent_window, bool modal) {
        return with_window(e, [&](W::Window &w) -> std::expected<void, W::WindowError> {
            const auto child = w.native_window_handle();
            if (!child) {
                return std::unexpected(child.error());
            }
            std::optional<W::NativeWindowHandle> parent;
            if (parent_window != ~std::uint64_t{0}) {
                parent = native_of(e, parent_window);
                if (!parent) {
                    return std::unexpected(W::WindowError{W::WindowErrorCode::InvalidArgument, "the parent window does not exist"});
                }
            }
            return W::window_result_from_effect_result(W::set_native_window_parent(*child, parent, modal));
        });
    }
    WindowStatus window_set_keyboard_grab(EngineView &e, bool enabled) {
        return with_window(e, [&](W::Window &w) { return w.set_keyboard_grab(enabled); });
    }
    WindowStatus window_set_mouse_confine_rect(EngineView &e, bool has_rect, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) {
        return with_window(e, [&](W::Window &w) {
            return w.set_mouse_confine_rect(has_rect ? std::optional<W::WindowRect>{W::WindowRect{x, y, width, height}} : std::nullopt);
        });
    }
    WindowStatus window_set_cursor_position(EngineView &e, std::int32_t x, std::int32_t y) {
        return with_window(e, [&](W::Window &w) { return w.set_cursor_position(W::WindowPosition{x, y}); });
    }
    WindowStatus window_set_global_cursor_position(EngineView &e, std::int32_t x, std::int32_t y) {
        return with_window(e, [&](W::Window &w) { return w.set_global_cursor_position(W::WindowPosition{x, y}); });
    }
    WindowStatus window_set_mouse_capture(EngineView &e, bool enabled) {
        return with_window(e, [&](W::Window &w) { return w.set_mouse_capture(enabled); });
    }
    WindowStatus window_set_custom_cursor(EngineView &e, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba,
                                          std::int32_t hot_x, std::int32_t hot_y) {
        return with_window(e, [&](W::Window &w) { return w.set_custom_cursor(W::CursorImage{width, height, as_bytes(rgba), hot_x, hot_y}); });
    }
    WindowStatus window_set_animated_cursor(EngineView &e, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> frames,
                                            rust::Slice<const std::uint32_t> durations_ms, std::int32_t hot_x, std::int32_t hot_y) {
        return with_window(e, [&](W::Window &w) -> std::expected<void, W::WindowError> {
            const std::size_t frame_bytes = static_cast<std::size_t>(width) * height * 4;
            if (frame_bytes == 0 || durations_ms.size() == 0 || frames.size() < frame_bytes * durations_ms.size()) {
                return std::unexpected(W::WindowError{W::WindowErrorCode::InvalidArgument, "an animated cursor needs one frame of pixels per duration"});
            }
            const auto all = as_bytes(frames);
            std::vector<W::CursorFrame> list;
            for (std::size_t i = 0; i < durations_ms.size(); ++i) {
                list.push_back(W::CursorFrame{W::CursorImage{width, height, all.subspan(i * frame_bytes, frame_bytes), hot_x, hot_y}, durations_ms[i]});
            }
            return w.set_animated_cursor(list);
        });
    }
    WindowStatus window_clear_custom_cursor(EngineView &e) {
        return with_window(e, [](W::Window &w) { return w.clear_custom_cursor(); });
    }
    WindowStatus window_set_shape(EngineView &e, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> coverage) {
        return with_window(e, [&](W::Window &w) { return w.set_shape(W::WindowShapeMask{width, height, as_bytes(coverage)}); });
    }
    WindowStatus window_set_window_type(EngineView &e, std::uint8_t window_type) {
        return with_window(e, [&](W::Window &w) { return w.set_window_type(static_cast<W::WindowType>(std::min<std::uint8_t>(window_type, 6))); });
    }
    WindowStatus window_set_display_gamma(EngineView &e, bool has_display, std::uint32_t display_id, float gamma) {
        return with_window(e, [&](W::Window &w) {
            return w.set_display_gamma(has_display ? std::optional<std::uint32_t>{display_id} : std::nullopt, gamma);
        });
    }
    WindowStatus window_set_fullscreen_target(EngineView &e, const FullscreenTargetInfo &target) {
        return with_window(e, [&](W::Window &w) { return w.set_fullscreen_target(to_target(target)); });
    }
    WindowStatus window_start_text_input_with(EngineView &e, std::uint8_t input_type, std::uint8_t capitalization, bool autocorrect, bool multiline) {
        return with_window(e, [&](W::Window &w) {
            return w.start_text_input_with(W::TextInputOptions{static_cast<W::TextInputType>(std::min<std::uint8_t>(input_type, 8)),
                                                                static_cast<W::TextCapitalization>(std::min<std::uint8_t>(capitalization, 3)),
                                                                autocorrect, multiline});
        });
    }
    rust::Vec<rust::String> window_clipboard_mime_types(EngineView &e) {
        rust::Vec<rust::String> out;
        if (W::Window *w = primary(e)) {
            for (const std::string &type : w->clipboard_mime_types()) {
                out.push_back(rust::String::lossy(type));
            }
        }
        return out;
    }
    rust::String window_key_name(EngineView &e, std::int32_t key) {
        W::Window *w = primary(e);
        return w != nullptr ? rust::String::lossy(w->key_name(static_cast<W::KeyboardKey>(key))) : rust::String();
    }
    bool window_key_down(EngineView &e, std::int32_t key) {
        W::Window *w = primary(e);
        return w != nullptr && w->key_down(static_cast<W::KeyboardKey>(key));
    }
    ProfileResult window_icc_profile(EngineView &e) {
        ProfileResult out{};
        W::Window *w = primary(e);
        if (w == nullptr) {
            out.error = rust::String("the engine has no primary window");
            return out;
        }
        auto profile = w->icc_profile();
        if (!profile) {
            out.error = rust::String::lossy(profile.error().message);
            return out;
        }
        out.ok = true;
        for (const std::byte b : *profile) {
            out.bytes.push_back(static_cast<std::uint8_t>(b));
        }
        return out;
    }

    std::uint32_t window_feature_count() { return static_cast<std::uint32_t>(W::WindowFeature::Count); }
    std::uint32_t window_operation_count() { return static_cast<std::uint32_t>(E::WindowOperation::Count); }

    // -- Any window, queued -----------------------------------------------------------------------------------------------

    WindowStatus window_run_command(EngineView &e, std::uint64_t window_id, const WindowCommand &c) {
        namespace C = SFT::Engine::WindowControl;
        auto &requests = e.window_requests();
        const auto id = static_cast<W::WindowId>(window_id);
        const auto ok = [] { return WindowStatus{true, rust::String()}; };
        const auto bad = [](const char *why) { return WindowStatus{false, rust::String(why)}; };
        if (c.op >= static_cast<std::uint32_t>(E::WindowOperation::Count)) {
            return bad("unrecognized window operation");
        }
        const auto op = static_cast<E::WindowOperation>(c.op);
        const auto extent = [&] { return W::WindowExtent{static_cast<std::uint32_t>(c.a), static_cast<std::uint32_t>(c.b)}; };
        const auto position = [&] { return W::WindowPosition{static_cast<std::int32_t>(c.a), static_cast<std::int32_t>(c.b)}; };
        switch (op) {
        case E::WindowOperation::Title: C::set_title(requests, id, std::string(c.text)); break;
        case E::WindowOperation::Position: C::set_position(requests, id, position()); break;
        case E::WindowOperation::Size: C::set_size(requests, id, extent()); break;
        case E::WindowOperation::MinimumSize: C::set_minimum_size(requests, id, extent()); break;
        case E::WindowOperation::MaximumSize: C::set_maximum_size(requests, id, extent()); break;
        case E::WindowOperation::Resizable: C::set_resizable(requests, id, c.flag); break;
        case E::WindowOperation::Opacity: C::set_opacity(requests, id, c.x); break;
        case E::WindowOperation::Maximize: C::maximize(requests, id); break;
        case E::WindowOperation::Minimize: C::minimize(requests, id); break;
        case E::WindowOperation::Restore: C::restore(requests, id); break;
        case E::WindowOperation::Raise: C::raise(requests, id); break;
        case E::WindowOperation::Focus: C::focus(requests, id); break;
        case E::WindowOperation::Show: C::show(requests, id); break;
        case E::WindowOperation::Hide: C::hide(requests, id); break;
        case E::WindowOperation::AlwaysOnTop: C::set_always_on_top(requests, id, c.flag); break;
        case E::WindowOperation::AspectRatio: C::set_aspect_ratio(requests, id, c.x, c.y); break;
        case E::WindowOperation::MousePassthrough: C::set_mouse_passthrough(requests, id, c.flag); break;
        case E::WindowOperation::Focusable: C::set_focusable(requests, id, c.flag); break;
        case E::WindowOperation::AutoMinimize: C::set_auto_minimize_on_focus_loss(requests, id, c.flag); break;
        case E::WindowOperation::Parent: {
            std::optional<W::NativeWindowHandle> parent;
            if (c.has_other) {
                parent = native_of(e, c.other_window);
                if (!parent) {
                    return bad("the parent window does not exist");
                }
            }
            C::set_parent(requests, id, parent, c.flag);
            break;
        }
        case E::WindowOperation::KeyboardGrab: C::set_keyboard_grab(requests, id, c.flag); break;
        case E::WindowOperation::MouseConfineRect:
            C::set_mouse_confine_rect(requests, id,
                                      c.flag ? std::optional<W::WindowRect>{W::WindowRect{static_cast<std::int32_t>(c.a), static_cast<std::int32_t>(c.b),
                                                                                         static_cast<std::int32_t>(c.c), static_cast<std::int32_t>(c.d)}}
                                             : std::nullopt);
            break;
        case E::WindowOperation::CursorPosition: C::set_cursor_position(requests, id, position()); break;
        case E::WindowOperation::GlobalCursorPosition: C::set_global_cursor_position(requests, id, position()); break;
        case E::WindowOperation::MouseCapture: C::set_mouse_capture(requests, id, c.flag); break;
        case E::WindowOperation::CustomCursor:
            C::set_custom_cursor(requests, id, static_cast<std::uint32_t>(c.a), static_cast<std::uint32_t>(c.b), copy_bytes(c.bytes),
                                 static_cast<std::int32_t>(c.c), static_cast<std::int32_t>(c.d));
            break;
        case E::WindowOperation::AnimatedCursor: {
            const std::size_t frame_bytes = static_cast<std::size_t>(c.a) * static_cast<std::size_t>(c.b) * 4;
            if (frame_bytes == 0 || c.durations.empty() || c.bytes.size() < frame_bytes * c.durations.size()) {
                return bad("an animated cursor needs one frame of pixels per duration");
            }
            const auto all = copy_bytes(c.bytes);
            std::vector<C::AnimatedCursorFrame> frames;
            for (std::size_t i = 0; i < c.durations.size(); ++i) {
                frames.push_back({std::vector<std::byte>(all.begin() + static_cast<std::ptrdiff_t>(i * frame_bytes),
                                                        all.begin() + static_cast<std::ptrdiff_t>((i + 1) * frame_bytes)),
                                  c.durations[i]});
            }
            C::set_animated_cursor(requests, id, static_cast<std::uint32_t>(c.a), static_cast<std::uint32_t>(c.b), std::move(frames),
                                   static_cast<std::int32_t>(c.c), static_cast<std::int32_t>(c.d));
            break;
        }
        case E::WindowOperation::ClearCustomCursor: C::clear_custom_cursor(requests, id); break;
        case E::WindowOperation::TextInputOptions:
            C::start_text_input_with(requests, id,
                                     W::TextInputOptions{static_cast<W::TextInputType>(std::clamp<std::int64_t>(c.a, 0, 8)),
                                                         static_cast<W::TextCapitalization>(std::clamp<std::int64_t>(c.b, 0, 3)), c.flag, c.c != 0});
            break;
        case E::WindowOperation::FullscreenTarget: {
            W::FullscreenTarget target;
            if (c.a >= 0) {
                target.display_id = static_cast<std::uint32_t>(c.a);
            }
            if (c.b > 0 && c.c > 0) {
                target.mode = W::DisplayMode{static_cast<std::uint32_t>(c.b), static_cast<std::uint32_t>(c.c), c.x, c.y > 0.0f ? c.y : 1.0f};
            }
            C::set_fullscreen_target(requests, id, target);
            break;
        }
        case E::WindowOperation::WindowType: C::set_window_type(requests, id, static_cast<W::WindowType>(std::clamp<std::int64_t>(c.a, 0, 6))); break;
        case E::WindowOperation::Shape:
            C::set_shape(requests, id, static_cast<std::uint32_t>(c.a), static_cast<std::uint32_t>(c.b), copy_bytes(c.bytes));
            break;
        case E::WindowOperation::DisplayGamma:
            C::set_display_gamma(requests, id, c.a >= 0 ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(c.a)} : std::nullopt, c.x);
            break;
        case E::WindowOperation::Icon:
            C::set_icon(requests, id, static_cast<std::uint32_t>(c.a), static_cast<std::uint32_t>(c.b), copy_bytes(c.bytes));
            break;
        case E::WindowOperation::Attention:
            C::request_attention(requests, id, static_cast<W::AttentionRequest>(std::clamp<std::int64_t>(c.a, 0, 2)));
            break;
        case E::WindowOperation::TaskbarProgress:
            C::set_taskbar_progress(requests, id, static_cast<W::TaskbarProgress>(std::clamp<std::int64_t>(c.a, 0, 4)), c.x);
            break;
        case E::WindowOperation::HitTestRegions:
            C::set_hit_test_regions(requests, id, hit_regions(c.flag, static_cast<std::uint32_t>(c.a), c.rects, c.no_drag_rects));
            break;
        case E::WindowOperation::SystemMenu: C::show_system_menu(requests, id, position()); break;
        case E::WindowOperation::ClipboardText: C::clipboard_text(requests, id); break;
        case E::WindowOperation::SetClipboardText: C::set_clipboard_text(requests, id, std::string(c.text)); break;
        case E::WindowOperation::ClipboardMimeTypes: C::clipboard_mime_types(requests, id); break;
        case E::WindowOperation::ClipboardData: C::clipboard_data(requests, id, std::string(c.text)); break;
        case E::WindowOperation::SetClipboardData: C::set_clipboard_data(requests, id, std::string(c.text), copy_bytes(c.bytes)); break;
        case E::WindowOperation::KeyName: C::key_name(requests, id, static_cast<W::KeyboardKey>(c.a)); break;
        case E::WindowOperation::KeyDown: C::key_down(requests, id, static_cast<W::KeyboardKey>(c.a)); break;
        case E::WindowOperation::IccProfile: C::icc_profile(requests, id); break;
        case E::WindowOperation::Count: return bad("unrecognized window operation");
        }
        return ok();
    }

    OperationResult window_operation_result(EngineView &e, std::uint64_t window_id, std::uint32_t op) {
        OperationResult out{};
        if (op >= static_cast<std::uint32_t>(E::WindowOperation::Count)) {
            return out;
        }
        const auto status = e.window_requests().operation_status(static_cast<W::WindowId>(window_id), static_cast<E::WindowOperation>(op));
        if (!status) {
            return out;
        }
        out.completed = true;
        out.ok = status->ok;
        out.error_kind = status->ok ? 0
                         : status->code == W::WindowErrorCode::Unsupported     ? 1
                         : status->code == W::WindowErrorCode::InvalidArgument ? 2
                                                                               : 3;
        out.message = rust::String::lossy(status->message);
        out.flag = status->output.flag;
        out.number = static_cast<std::int32_t>(status->output.number);
        out.text = rust::String::lossy(status->output.text);
        for (const std::byte b : status->output.bytes) {
            out.bytes.push_back(static_cast<std::uint8_t>(b));
        }
        for (const std::string &item : status->output.list) {
            out.list.push_back(rust::String::lossy(item));
        }
        return out;
    }

    // -- Desktop services -------------------------------------------------------------------------------------------------

    namespace {

        WindowStatus desktop_status(const std::expected<void, W::WindowError> &result) { return status(result); }

        struct DialogTicket {
            bool ready = false;
            W::Desktop::FileDialogResult result;
        };
        std::mutex g_dialog_mutex;
        std::map<std::uint64_t, std::shared_ptr<DialogTicket>> g_dialogs;
        std::uint64_t g_next_dialog = 1;

        struct TrayState {
            std::unique_ptr<W::Desktop::Tray> tray;
            std::mutex clicks_mutex;
            std::vector<std::uint32_t> clicks;
        };
        std::mutex g_tray_mutex;
        std::map<std::uint64_t, std::shared_ptr<TrayState>> g_trays;
        std::uint64_t g_next_tray = 1;

        /// Builds the menu tree from the flat rows (`parent` indexes an earlier row); clicks are queued for Rust to poll.
        std::vector<W::Desktop::TrayEntry> build_tray(rust::Slice<const TrayEntryInfo> rows, const std::shared_ptr<TrayState> &state, std::int32_t parent) {
            std::vector<W::Desktop::TrayEntry> out;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const TrayEntryInfo &row = rows[i];
                if (row.parent != parent) {
                    continue;
                }
                W::Desktop::TrayEntry entry;
                entry.label = std::string(row.label);
                entry.separator = row.separator;
                entry.checkable = row.checkable;
                entry.checked = row.checked;
                entry.enabled = row.enabled;
                entry.submenu = build_tray(rows, state, static_cast<std::int32_t>(i));
                if (entry.submenu.empty()) {
                    entry.on_click = [weak = std::weak_ptr<TrayState>(state), id = row.id] {
                        if (const auto locked = weak.lock()) {
                            const std::lock_guard lock{locked->clicks_mutex};
                            locked->clicks.push_back(id);
                        }
                    };
                }
                out.push_back(std::move(entry));
            }
            return out;
        }

        /// Registers a ticket and hands `launch` the options and a callback that completes it. 0 when launching failed.
        template <typename Launch>
        std::uint64_t begin_dialog(W::Desktop::FileDialogOptions options, rust::String &error, Launch &&launch) {
            auto ticket = std::make_shared<DialogTicket>();
            std::uint64_t number = 0;
            {
                const std::lock_guard lock{g_dialog_mutex};
                number = g_next_dialog++;
                g_dialogs.emplace(number, ticket);
            }
            // The dialog finishes on whatever thread SDL uses; the ticket is read back by polling.
            const auto started = launch(std::move(options), [ticket](W::Desktop::FileDialogResult result) {
                const std::lock_guard lock{g_dialog_mutex};
                ticket->result = std::move(result);
                ticket->ready = true;
            });
            if (!started) {
                error = rust::String::lossy(started.error().message);
                const std::lock_guard lock{g_dialog_mutex};
                g_dialogs.erase(number);
                return 0;
            }
            return number;
        }

        std::shared_ptr<TrayState> find_tray(std::uint64_t handle) {
            const std::lock_guard lock{g_tray_mutex};
            const auto found = g_trays.find(handle);
            return found != g_trays.end() ? found->second : nullptr;
        }

    } // namespace

    // -- Per-tick input state ------------------------------------------------------------------------------------------------

    rust::Vec<TouchPointInfo> input_touches(const EngineView &e) {
        rust::Vec<TouchPointInfo> out;
        for (const auto &t : e.input_state().touches()) {
            out.push_back(TouchPointInfo{static_cast<std::uint64_t>(t.window), t.device, t.finger, t.x, t.y, t.dx, t.dy, t.pressure, t.down,
                                         t.just_pressed, t.just_released, t.cancelled});
        }
        return out;
    }

    rust::Vec<PenPointInfo> input_pens(const EngineView &e) {
        rust::Vec<PenPointInfo> out;
        for (const auto &p : e.input_state().pens()) {
            out.push_back(PenPointInfo{static_cast<std::uint64_t>(p.window), p.pen, p.x, p.y, p.in_range, p.down, p.eraser, p.just_down, p.just_up,
                                       p.pressure, p.x_tilt, p.y_tilt, p.distance, p.rotation, p.slider, p.tangential_pressure, p.buttons});
        }
        return out;
    }

    void input_set_gamepads_enabled(EngineView &e, bool enabled) { e.input_state().set_gamepads_enabled(enabled); }

    rust::Vec<GamepadFrameInfo> input_gamepads(const EngineView &e) {
        rust::Vec<GamepadFrameInfo> out;
        for (const auto &frame : e.input_state().gamepads()) {
            GamepadFrameInfo info{};
            info.id = frame.id;
            info.buttons = frame.state.buttons;
            info.pressed_this_tick = frame.pressed_this_tick;
            info.released_this_tick = frame.released_this_tick;
            for (std::size_t i = 0; i < frame.state.axes.size(); ++i) {
                info.axes[i] = frame.state.axes[i];
            }
            out.push_back(info);
        }
        return out;
    }

    rust::Vec<GamepadEventBridge> input_gamepad_events(const EngineView &e) {
        rust::Vec<GamepadEventBridge> out;
        for (const auto &event : e.input_state().gamepad_events()) {
            out.push_back(GamepadEventBridge{static_cast<std::uint8_t>(event.kind), event.id});
        }
        return out;
    }

    // -- Gamepads ----------------------------------------------------------------------------------------------------------

    rust::Vec<GamepadInfoBridge> gamepad_list(rust::String &error) {
        namespace G = W::Gamepads;
        rust::Vec<GamepadInfoBridge> out;
        const auto devices = G::list();
        if (!devices) {
            error = rust::String::lossy(devices.error().message);
            return out;
        }
        for (const G::Info &d : *devices) {
            GamepadInfoBridge info{};
            info.id = d.id;
            info.name = rust::String::lossy(d.name);
            info.kind = static_cast<std::uint8_t>(d.kind);
            info.vendor = d.vendor;
            info.product = d.product;
            info.connection = static_cast<std::uint8_t>(d.connection);
            info.player_index = d.player_index;
            info.mapped = d.mapped;
            info.has_rumble = d.has_rumble;
            info.has_trigger_rumble = d.has_trigger_rumble;
            info.has_led = d.has_led;
            info.has_accelerometer = d.has_accelerometer;
            info.has_gyroscope = d.has_gyroscope;
            info.has_touchpad = d.has_touchpad;
            info.battery_percent = d.battery_percent;
            info.charging = d.charging;
            out.push_back(std::move(info));
        }
        return out;
    }

    GamepadStateBridge gamepad_state(std::uint32_t id) {
        GamepadStateBridge out{};
        if (const auto state = W::Gamepads::state(id)) {
            out.available = true;
            out.buttons = state->buttons;
            for (std::size_t i = 0; i < state->axes.size(); ++i) {
                out.axes[i] = state->axes[i];
            }
        }
        return out;
    }

    GamepadRawBridge gamepad_raw_state(std::uint32_t id) {
        GamepadRawBridge out{};
        if (const auto raw = W::Gamepads::raw_state(id)) {
            out.available = true;
            for (const float axis : raw->axes) {
                out.axes.push_back(axis);
            }
            for (const std::uint8_t button : raw->buttons) {
                out.buttons.push_back(button);
            }
            for (const std::uint8_t hat : raw->hats) {
                out.hats.push_back(hat);
            }
        }
        return out;
    }

    rust::Vec<GamepadEventBridge> gamepad_take_events() {
        rust::Vec<GamepadEventBridge> out;
        for (const W::Gamepads::Event &event : W::Gamepads::take_events()) {
            out.push_back(GamepadEventBridge{static_cast<std::uint8_t>(event.kind), event.id});
        }
        return out;
    }

    WindowStatus gamepad_rumble(std::uint32_t id, float low, float high, std::uint32_t duration_ms) {
        return desktop_status(W::Gamepads::rumble(id, low, high, duration_ms));
    }
    WindowStatus gamepad_rumble_triggers(std::uint32_t id, float left, float right, std::uint32_t duration_ms) {
        return desktop_status(W::Gamepads::rumble_triggers(id, left, right, duration_ms));
    }
    WindowStatus gamepad_set_led(std::uint32_t id, std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
        return desktop_status(W::Gamepads::set_led(id, red, green, blue));
    }
    WindowStatus gamepad_set_player_index(std::uint32_t id, std::int32_t player_index) {
        return desktop_status(W::Gamepads::set_player_index(id, player_index));
    }
    WindowStatus gamepad_set_sensor_enabled(std::uint32_t id, std::uint8_t sensor, bool enabled) {
        return desktop_status(W::Gamepads::set_sensor_enabled(id, sensor == 0 ? W::Gamepads::SensorKind::Accelerometer : W::Gamepads::SensorKind::Gyroscope, enabled));
    }
    SensorReading gamepad_sensor(std::uint32_t id, std::uint8_t sensor) {
        SensorReading out{};
        const auto data = W::Gamepads::sensor(id, sensor == 0 ? W::Gamepads::SensorKind::Accelerometer : W::Gamepads::SensorKind::Gyroscope);
        if (data) {
            out.ok = true;
            out.xyz = {(*data)[0], (*data)[1], (*data)[2]};
        }
        return out;
    }

    WindowStatus desktop_set_screensaver_enabled(bool enabled) { return desktop_status(W::Desktop::set_screensaver_enabled(enabled)); }

    WindowStatus desktop_screensaver_enabled(bool &out) {
        const auto value = W::Desktop::screensaver_enabled();
        if (!value) {
            return WindowStatus{false, rust::String::lossy(value.error().message)};
        }
        out = *value;
        return WindowStatus{true, rust::String()};
    }

    MessageBoxResult desktop_message_box(std::uint8_t kind, rust::Str title, rust::Str message, rust::Slice<const rust::String> buttons,
                                         std::int32_t default_button, std::int32_t escape_button) {
        W::Desktop::MessageBoxOptions options;
        options.kind = static_cast<W::Desktop::MessageBoxKind>(std::min<std::uint8_t>(kind, 2));
        options.title = std::string(title);
        options.message = std::string(message);
        for (const rust::String &button : buttons) {
            options.buttons.emplace_back(std::string(button));
        }
        options.default_button = default_button;
        options.escape_button = escape_button;
        const auto pressed = W::Desktop::show_message_box(options);
        if (!pressed) {
            return MessageBoxResult{false, rust::String::lossy(pressed.error().message), -1};
        }
        return MessageBoxResult{true, rust::String(), *pressed};
    }

    std::uint64_t desktop_file_dialog_start(std::uint8_t kind, rust::Str title, rust::Str accept_label, rust::Str cancel_label,
                                            rust::Str default_location, rust::Slice<const rust::String> filter_names,
                                            rust::Slice<const rust::String> filter_patterns, bool allow_multiple, rust::String &error) {
        W::Desktop::FileDialogOptions options;
        options.kind = static_cast<W::Desktop::FileDialogKind>(std::min<std::uint8_t>(kind, 2));
        options.title = std::string(title);
        options.accept_label = std::string(accept_label);
        options.cancel_label = std::string(cancel_label);
        options.default_location = std::string(default_location);
        options.allow_multiple = allow_multiple;
        for (std::size_t i = 0; i < filter_names.size() && i < filter_patterns.size(); ++i) {
            options.filters.push_back({std::string(filter_names[i]), std::string(filter_patterns[i])});
        }
        return begin_dialog(std::move(options), error, [](W::Desktop::FileDialogOptions o, W::Desktop::FileDialogCallback done) {
            return W::Desktop::show_file_dialog(std::move(o), std::move(done));
        });
    }

    std::uint64_t desktop_file_dialog_start_for(EngineView &e, std::uint64_t window_id, std::uint8_t kind, rust::Str title, rust::Str accept_label,
                                                rust::Str cancel_label, rust::Str default_location, rust::Slice<const rust::String> filter_names,
                                                rust::Slice<const rust::String> filter_patterns, bool allow_multiple, rust::String &error) {
        W::Desktop::FileDialogOptions options;
        options.kind = static_cast<W::Desktop::FileDialogKind>(std::min<std::uint8_t>(kind, 2));
        options.title = std::string(title);
        options.accept_label = std::string(accept_label);
        options.cancel_label = std::string(cancel_label);
        options.default_location = std::string(default_location);
        options.allow_multiple = allow_multiple;
        for (std::size_t i = 0; i < filter_names.size() && i < filter_patterns.size(); ++i) {
            options.filters.push_back({std::string(filter_names[i]), std::string(filter_patterns[i])});
        }
        // Queued: the dialog opens on the window's thread, parented to that window.
        return begin_dialog(std::move(options), error, [&e, window_id](W::Desktop::FileDialogOptions o, W::Desktop::FileDialogCallback done) {
            SFT::Engine::WindowControl::show_file_dialog(e.window_requests(), static_cast<W::WindowId>(window_id), std::move(o), std::move(done));
            return std::expected<void, W::WindowError>{};
        });
    }

    WindowStatus window_show_message_box(EngineView &e, std::uint64_t window_id, std::uint8_t kind, rust::Str title, rust::Str message,
                                         rust::Slice<const rust::String> buttons, std::int32_t default_button, std::int32_t escape_button) {
        W::Desktop::MessageBoxOptions options;
        options.kind = static_cast<W::Desktop::MessageBoxKind>(std::min<std::uint8_t>(kind, 2));
        options.title = std::string(title);
        options.message = std::string(message);
        for (const rust::String &button : buttons) {
            options.buttons.emplace_back(std::string(button));
        }
        options.default_button = default_button;
        options.escape_button = escape_button;
        SFT::Engine::WindowControl::show_message_box(e.window_requests(), static_cast<W::WindowId>(window_id), std::move(options));
        return WindowStatus{true, rust::String()};
    }

    FileDialogOutcome desktop_file_dialog_take(std::uint64_t ticket) {
        FileDialogOutcome out{};
        const std::lock_guard lock{g_dialog_mutex};
        const auto found = g_dialogs.find(ticket);
        if (found == g_dialogs.end() || !found->second->ready) {
            return out;
        }
        const W::Desktop::FileDialogResult &result = found->second->result;
        out.ready = true;
        out.cancelled = result.cancelled;
        out.failed = result.failed;
        out.error = rust::String::lossy(result.error);
        out.filter_index = result.filter_index;
        for (const std::string &path : result.paths) {
            out.paths.push_back(rust::String::lossy(path));
        }
        g_dialogs.erase(found);
        return out;
    }

    void desktop_pump_events() { W::Desktop::pump_events(); }

    TrayResult tray_create(std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba, rust::Str tooltip,
                           rust::Slice<const TrayEntryInfo> entries) {
        auto state = std::make_shared<TrayState>();
        auto created = W::Desktop::Tray::create(width, height, as_bytes(rgba), std::string(tooltip), build_tray(entries, state, -1));
        if (!created) {
            return TrayResult{false, rust::String::lossy(created.error().message), 0};
        }
        state->tray = std::move(*created);
        const std::lock_guard lock{g_tray_mutex};
        const std::uint64_t handle = g_next_tray++;
        g_trays.emplace(handle, std::move(state));
        return TrayResult{true, rust::String(), handle};
    }

    WindowStatus tray_set_icon(std::uint64_t handle, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba) {
        const auto state = find_tray(handle);
        return state ? desktop_status(state->tray->set_icon(width, height, as_bytes(rgba))) : WindowStatus{false, rust::String("unknown tray")};
    }

    WindowStatus tray_set_tooltip(std::uint64_t handle, rust::Str tooltip) {
        const auto state = find_tray(handle);
        if (!state) {
            return WindowStatus{false, rust::String("unknown tray")};
        }
        state->tray->set_tooltip(std::string(tooltip));
        return WindowStatus{true, rust::String()};
    }

    WindowStatus tray_set_menu(std::uint64_t handle, rust::Slice<const TrayEntryInfo> entries) {
        const auto state = find_tray(handle);
        return state ? desktop_status(state->tray->set_menu(build_tray(entries, state, -1))) : WindowStatus{false, rust::String("unknown tray")};
    }

    rust::Vec<std::uint32_t> tray_take_clicks(std::uint64_t handle) {
        rust::Vec<std::uint32_t> out;
        if (const auto state = find_tray(handle)) {
            const std::lock_guard lock{state->clicks_mutex};
            for (const std::uint32_t id : state->clicks) {
                out.push_back(id);
            }
            state->clicks.clear();
        }
        return out;
    }

    WindowStatus tray_destroy(std::uint64_t handle) {
        std::shared_ptr<TrayState> state;
        {
            const std::lock_guard lock{g_tray_mutex};
            const auto found = g_trays.find(handle);
            if (found == g_trays.end()) {
                return WindowStatus{false, rust::String("unknown tray")};
            }
            state = std::move(found->second);
            g_trays.erase(found);
        }
        return WindowStatus{true, rust::String()};
    }

    rust::Vec<DropEventInfo> take_drop_events(EngineView &e) {
        rust::Vec<DropEventInfo> out;
        for (const SFT::Engine::Engine::DropEvent &drop : e.take_drop_events()) {
            DropEventInfo info{};
            info.window_id = static_cast<std::uint64_t>(drop.window);
            info.kind = static_cast<std::uint8_t>(drop.kind);
            info.x = drop.position.x;
            info.y = drop.position.y;
            info.data = rust::String::lossy(drop.data);
            out.push_back(std::move(info));
        }
        return out;
    }

} // namespace sturdy_rs::window
