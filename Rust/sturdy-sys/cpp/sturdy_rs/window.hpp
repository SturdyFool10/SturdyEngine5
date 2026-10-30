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

    struct FeatureInfo;
    struct WindowGeometryInfo;
    struct FullscreenTargetInfo;
    struct ProfileResult;
    struct WindowCommand;
    struct OperationResult;
    struct MessageBoxResult;
    struct FileDialogOutcome;
    struct TrayEntryInfo;
    struct TrayResult;

    FeatureInfo window_feature(EngineView &engine, std::uint32_t feature);
    WindowGeometryInfo window_geometry(EngineView &engine);
    WindowStatus window_set_always_on_top(EngineView &engine, bool enabled);
    WindowStatus window_set_aspect_ratio(EngineView &engine, float min_ratio, float max_ratio);
    WindowStatus window_set_mouse_passthrough(EngineView &engine, bool enabled);
    WindowStatus window_set_focusable(EngineView &engine, bool enabled);
    WindowStatus window_set_auto_minimize_on_focus_loss(EngineView &engine, bool enabled);
    WindowStatus window_set_parent(EngineView &engine, std::uint64_t parent_window, bool modal);
    WindowStatus window_set_keyboard_grab(EngineView &engine, bool enabled);
    WindowStatus window_set_mouse_confine_rect(EngineView &engine, bool has_rect, std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height);
    WindowStatus window_set_cursor_position(EngineView &engine, std::int32_t x, std::int32_t y);
    WindowStatus window_set_global_cursor_position(EngineView &engine, std::int32_t x, std::int32_t y);
    WindowStatus window_set_mouse_capture(EngineView &engine, bool enabled);
    WindowStatus window_set_custom_cursor(EngineView &engine, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba,
                                          std::int32_t hot_x, std::int32_t hot_y);
    WindowStatus window_set_animated_cursor(EngineView &engine, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> frames,
                                            rust::Slice<const std::uint32_t> durations_ms, std::int32_t hot_x, std::int32_t hot_y);
    WindowStatus window_clear_custom_cursor(EngineView &engine);
    WindowStatus window_set_shape(EngineView &engine, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> coverage);
    WindowStatus window_set_window_type(EngineView &engine, std::uint8_t window_type);
    WindowStatus window_set_display_gamma(EngineView &engine, bool has_display, std::uint32_t display_id, float gamma);
    WindowStatus window_set_fullscreen_target(EngineView &engine, const FullscreenTargetInfo &target);
    WindowStatus window_start_text_input_with(EngineView &engine, std::uint8_t input_type, std::uint8_t capitalization, bool autocorrect, bool multiline);
    rust::Vec<rust::String> window_clipboard_mime_types(EngineView &engine);
    rust::String window_key_name(EngineView &engine, std::int32_t key);
    bool window_key_down(EngineView &engine, std::int32_t key);
    ProfileResult window_icc_profile(EngineView &engine);

    std::uint32_t window_feature_count();
    std::uint32_t window_operation_count();

    WindowStatus window_run_command(EngineView &engine, std::uint64_t window_id, const WindowCommand &command);
    OperationResult window_operation_result(EngineView &engine, std::uint64_t window_id, std::uint32_t op);
    FeatureInfo window_feature_of(EngineView &engine, std::uint64_t window_id, std::uint32_t feature);

    struct GamepadEventBridge;
    struct TouchPointInfo;
    struct PenPointInfo;
    struct GamepadFrameInfo;

    rust::Vec<TouchPointInfo> input_touches(const EngineView &engine);
    rust::Vec<PenPointInfo> input_pens(const EngineView &engine);
    void input_set_gamepads_enabled(EngineView &engine, bool enabled);
    rust::Vec<GamepadFrameInfo> input_gamepads(const EngineView &engine);
    rust::Vec<GamepadEventBridge> input_gamepad_events(const EngineView &engine);

    struct GamepadInfoBridge;
    struct GamepadStateBridge;
    struct GamepadRawBridge;
    struct GamepadEventBridge;
    struct SensorReading;

    rust::Vec<GamepadInfoBridge> gamepad_list(rust::String &error);
    GamepadStateBridge gamepad_state(std::uint32_t id);
    GamepadRawBridge gamepad_raw_state(std::uint32_t id);
    rust::Vec<GamepadEventBridge> gamepad_take_events();
    WindowStatus gamepad_rumble(std::uint32_t id, float low, float high, std::uint32_t duration_ms);
    WindowStatus gamepad_rumble_triggers(std::uint32_t id, float left, float right, std::uint32_t duration_ms);
    WindowStatus gamepad_set_led(std::uint32_t id, std::uint8_t red, std::uint8_t green, std::uint8_t blue);
    WindowStatus gamepad_set_player_index(std::uint32_t id, std::int32_t player_index);
    WindowStatus gamepad_set_sensor_enabled(std::uint32_t id, std::uint8_t sensor, bool enabled);
    SensorReading gamepad_sensor(std::uint32_t id, std::uint8_t sensor);

    WindowStatus desktop_set_screensaver_enabled(bool enabled);
    WindowStatus desktop_screensaver_enabled(bool &out);
    MessageBoxResult desktop_message_box(std::uint8_t kind, rust::Str title, rust::Str message, rust::Slice<const rust::String> buttons,
                                         std::int32_t default_button, std::int32_t escape_button);
    std::uint64_t desktop_file_dialog_start(std::uint8_t kind, rust::Str title, rust::Str accept_label, rust::Str cancel_label,
                                            rust::Str default_location, rust::Slice<const rust::String> filter_names,
                                            rust::Slice<const rust::String> filter_patterns, bool allow_multiple, rust::String &error);
    FileDialogOutcome desktop_file_dialog_take(std::uint64_t ticket);
    std::uint64_t desktop_file_dialog_start_for(EngineView &engine, std::uint64_t window_id, std::uint8_t kind, rust::Str title, rust::Str accept_label,
                                                rust::Str cancel_label, rust::Str default_location, rust::Slice<const rust::String> filter_names,
                                                rust::Slice<const rust::String> filter_patterns, bool allow_multiple, rust::String &error);
    WindowStatus window_show_message_box(EngineView &engine, std::uint64_t window_id, std::uint8_t kind, rust::Str title, rust::Str message,
                                         rust::Slice<const rust::String> buttons, std::int32_t default_button, std::int32_t escape_button);
    void desktop_pump_events();
    TrayResult tray_create(std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba, rust::Str tooltip,
                           rust::Slice<const TrayEntryInfo> entries);
    WindowStatus tray_set_icon(std::uint64_t handle, std::uint32_t width, std::uint32_t height, rust::Slice<const std::uint8_t> rgba);
    WindowStatus tray_set_tooltip(std::uint64_t handle, rust::Str tooltip);
    WindowStatus tray_set_menu(std::uint64_t handle, rust::Slice<const TrayEntryInfo> entries);
    rust::Vec<std::uint32_t> tray_take_clicks(std::uint64_t handle);
    WindowStatus tray_destroy(std::uint64_t handle);

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
