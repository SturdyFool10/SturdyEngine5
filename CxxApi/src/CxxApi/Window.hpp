#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Engine/Engine.hpp>

#include <CxxApi/Types/Window.hpp>

namespace SFT::CxxApi {

    /// A new window's engine defaults (the Rust `Default` for `WindowDesc`).
    [[nodiscard]] WindowDesc window_desc_defaults() noexcept;

    // ---- the primary window, called synchronously (use from the game-logic thread) ----
    // Every fallible call throws when the engine has no primary window or the platform refuses.

    [[nodiscard]] bool window_has_primary(Engine::Engine &engine) noexcept;
    [[nodiscard]] bool window_close_requested(Engine::Engine &engine) noexcept;
    void window_request_close(Engine::Engine &engine) noexcept;
    void window_show(Engine::Engine &engine);
    void window_hide(Engine::Engine &engine);
    void window_focus(Engine::Engine &engine);
    void window_raise(Engine::Engine &engine);
    void window_maximize(Engine::Engine &engine);
    void window_minimize(Engine::Engine &engine);
    void window_restore(Engine::Engine &engine);
    void window_set_title(Engine::Engine &engine, const std::string &title);
    [[nodiscard]] Int2Result window_position(Engine::Engine &engine);
    void window_set_position(Engine::Engine &engine, std::int32_t x, std::int32_t y);
    [[nodiscard]] Int2Result window_global_cursor_position(Engine::Engine &engine);
    /// Window coordinates.
    [[nodiscard]] Int2Result window_size(Engine::Engine &engine);
    /// Physical pixels.
    [[nodiscard]] Int2Result window_framebuffer_size(Engine::Engine &engine);
    /// Physical pixels.
    void window_set_size(Engine::Engine &engine, std::uint32_t width, std::uint32_t height);
    void window_set_minimum_size(Engine::Engine &engine, std::uint32_t width, std::uint32_t height);
    void window_set_maximum_size(Engine::Engine &engine, std::uint32_t width, std::uint32_t height);
    void window_set_resizable(Engine::Engine &engine, bool resizable);
    [[nodiscard]] WindowMode window_fullscreen_mode(Engine::Engine &engine) noexcept;
    void window_set_opacity(Engine::Engine &engine, float opacity);
    [[nodiscard]] float window_opacity(Engine::Engine &engine);
    void window_set_cursor_visible(Engine::Engine &engine, bool visible);
    [[nodiscard]] float window_content_scale(Engine::Engine &engine) noexcept;
    [[nodiscard]] float window_refresh_rate_hz(Engine::Engine &engine);
    [[nodiscard]] std::unique_ptr<std::string> window_clipboard_text(Engine::Engine &engine);
    void window_set_clipboard_text(Engine::Engine &engine, const std::string &text);
    [[nodiscard]] HdrProperties window_hdr_properties(Engine::Engine &engine);

    // ---- every managed window, as observed at the start of the current tick ----

    [[nodiscard]] std::size_t window_count(const Engine::Engine &engine) noexcept;
    /// Throws when `index >= window_count`.
    [[nodiscard]] WindowSnapshot window_snapshot_at(const Engine::Engine &engine, std::size_t index);
    /// Fills `out` and returns true when `window` is managed.
    [[nodiscard]] bool window_find(const Engine::Engine &engine, std::uint64_t window, WindowSnapshot &out) noexcept;
    /// Returns false when there is no primary window.
    [[nodiscard]] bool window_primary_snapshot(const Engine::Engine &engine, WindowSnapshot &out) noexcept;

    // ---- queued requests (any window; applied between ticks) ----

    /// Queues a new window and returns the request id; see `window_take_completions`. Throws when no
    /// graphics device is active.
    [[nodiscard]] std::uint64_t window_request_spawn(Engine::Engine &engine, const WindowDesc &desc, const std::string &title);
    [[nodiscard]] std::uint64_t window_request_recreate_primary(Engine::Engine &engine, const WindowDesc &desc, const std::string &title);
    [[nodiscard]] std::uint64_t window_request_close_window(Engine::Engine &engine, std::uint64_t window);
    void window_request_cursor_icon(Engine::Engine &engine, std::uint64_t window, CursorIcon icon);
    void window_request_fullscreen(Engine::Engine &engine, std::uint64_t window, WindowMode mode);
    void window_request_decorated(Engine::Engine &engine, std::uint64_t window, bool decorated);
    void window_request_transparent(Engine::Engine &engine, std::uint64_t window, bool transparent);
    void window_request_relative_mouse_mode(Engine::Engine &engine, std::uint64_t window, bool enabled);
    void window_request_mouse_locked(Engine::Engine &engine, std::uint64_t window, bool locked);
    void window_request_cursor_grabbed(Engine::Engine &engine, std::uint64_t window, bool grabbed);
    void window_request_effect(Engine::Engine &engine, std::uint64_t window, WindowEffect effect, bool enabled);
    /// `window_request_effect` with every per-effect setting (colours, tint, opacity, corner preference, macOS options).
    void window_request_effect_ex(Engine::Engine &engine, std::uint64_t window, WindowEffect effect, bool enabled,
                                  const WindowEffectOptions &options);
    // ---- the union surface, for any window (queued; read outcomes with `window_operation_status`) ----
    // The window layer is the union of what its providers (SDL3, GLFW) can do. Ask first with `window_feature_supported`
    // (and read why not with `window_feature_note`), or just try and look for an `error` of 1 (unsupported).

    [[nodiscard]] bool window_feature_supported(Engine::Engine &engine, std::uint64_t window, WindowFeature feature);
    [[nodiscard]] std::unique_ptr<std::string> window_feature_note(Engine::Engine &engine, std::uint64_t window, WindowFeature feature);
    [[nodiscard]] WindowGeometryInfo window_geometry(Engine::Engine &engine, std::uint64_t window);
    [[nodiscard]] WindowOperationInfo window_operation_status(Engine::Engine &engine, std::uint64_t window, WindowOperation operation);
    [[nodiscard]] std::unique_ptr<std::string> window_operation_message(Engine::Engine &engine, std::uint64_t window, WindowOperation operation);
    /// A query's text, bytes or list output.
    [[nodiscard]] std::unique_ptr<std::string> window_operation_text(Engine::Engine &engine, std::uint64_t window, WindowOperation operation);
    [[nodiscard]] std::unique_ptr<std::vector<std::uint8_t>> window_operation_bytes(Engine::Engine &engine, std::uint64_t window,
                                                                                    WindowOperation operation);
    [[nodiscard]] std::unique_ptr<std::vector<std::string>> window_operation_list(Engine::Engine &engine, std::uint64_t window,
                                                                                  WindowOperation operation);

    void window_queue_set_title(Engine::Engine &engine, std::uint64_t window, const std::string &title);
    void window_queue_set_position(Engine::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y);
    void window_queue_set_size(Engine::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height);
    void window_queue_set_minimum_size(Engine::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height);
    void window_queue_set_maximum_size(Engine::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height);
    void window_queue_set_resizable(Engine::Engine &engine, std::uint64_t window, bool resizable);
    void window_queue_set_opacity(Engine::Engine &engine, std::uint64_t window, float opacity);
    void window_queue_maximize(Engine::Engine &engine, std::uint64_t window);
    void window_queue_minimize(Engine::Engine &engine, std::uint64_t window);
    void window_queue_restore(Engine::Engine &engine, std::uint64_t window);
    void window_queue_raise(Engine::Engine &engine, std::uint64_t window);
    void window_queue_focus(Engine::Engine &engine, std::uint64_t window);
    void window_queue_set_always_on_top(Engine::Engine &engine, std::uint64_t window, bool enabled);
    /// Width / height limits; 0 and 0 remove the constraint. GLFW holds one exact ratio.
    void window_queue_set_aspect_ratio(Engine::Engine &engine, std::uint64_t window, float min_ratio, float max_ratio);
    void window_queue_set_mouse_passthrough(Engine::Engine &engine, std::uint64_t window, bool enabled);
    void window_queue_set_focusable(Engine::Engine &engine, std::uint64_t window, bool enabled);
    void window_queue_set_auto_minimize_on_focus_loss(Engine::Engine &engine, std::uint64_t window, bool enabled);
    /// `parent` is another window's id; `has_parent` false detaches.
    void window_queue_set_parent(Engine::Engine &engine, std::uint64_t window, bool has_parent, std::uint64_t parent, bool modal);
    void window_queue_set_keyboard_grab(Engine::Engine &engine, std::uint64_t window, bool enabled);
    void window_queue_set_mouse_confine_rect(Engine::Engine &engine, std::uint64_t window, bool has_rect, std::int32_t x, std::int32_t y,
                                             std::int32_t width, std::int32_t height);
    void window_queue_set_cursor_position(Engine::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y);
    void window_queue_set_global_cursor_position(Engine::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y);
    void window_queue_set_mouse_capture(Engine::Engine &engine, std::uint64_t window, bool enabled);
    void window_queue_set_custom_cursor(Engine::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                                        const std::vector<std::uint8_t> &rgba, std::int32_t hot_x, std::int32_t hot_y);
    /// `frames` is every frame's RGBA8 pixels back to back, `durations_ms` has one entry per frame.
    void window_queue_set_animated_cursor(Engine::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                                          const std::vector<std::uint8_t> &frames, const std::vector<std::uint32_t> &durations_ms,
                                          std::int32_t hot_x, std::int32_t hot_y);
    void window_queue_clear_custom_cursor(Engine::Engine &engine, std::uint64_t window);
    /// `display_id` of 0 with `has_display` false means "the window's display"; a zero `mode_width` means "the desktop mode".
    void window_queue_set_fullscreen_target(Engine::Engine &engine, std::uint64_t window, bool has_display, std::uint32_t display_id,
                                            std::uint32_t mode_width, std::uint32_t mode_height, float refresh_hz);
    void window_queue_set_window_type(Engine::Engine &engine, std::uint64_t window, WindowType window_type);
    /// An empty `coverage` restores the rectangle.
    void window_queue_set_shape(Engine::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                                const std::vector<std::uint8_t> &coverage);
    void window_queue_set_icon(Engine::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                               const std::vector<std::uint8_t> &rgba);
    /// 0 cancel, 1 briefly, 2 until focused.
    void window_queue_request_attention(Engine::Engine &engine, std::uint64_t window, std::uint8_t request);
    /// `state` 0 none, 1 indeterminate, 2 normal, 3 paused, 4 error.
    void window_queue_set_taskbar_progress(Engine::Engine &engine, std::uint64_t window, std::uint8_t state, float value);
    void window_queue_show_system_menu(Engine::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y);
    void window_queue_set_display_gamma(Engine::Engine &engine, std::uint64_t window, bool has_display, std::uint32_t display_id, float gamma);
    void window_queue_query_clipboard_text(Engine::Engine &engine, std::uint64_t window);
    void window_queue_set_clipboard_text(Engine::Engine &engine, std::uint64_t window, const std::string &text);
    void window_queue_query_clipboard_mime_types(Engine::Engine &engine, std::uint64_t window);
    void window_queue_query_key_name(Engine::Engine &engine, std::uint64_t window, std::int32_t key);
    void window_queue_query_key_down(Engine::Engine &engine, std::uint64_t window, std::int32_t key);
    void window_queue_query_icc_profile(Engine::Engine &engine, std::uint64_t window);
    /// A message box over the window. It blocks that window's thread until dismissed; the pressed button arrives as
    /// `window_operation_status(..., WindowOperation::MessageBox).number`. `kind` 0 information, 1 warning, 2 error.
    void window_queue_show_message_box(Engine::Engine &engine, std::uint64_t window, std::uint8_t kind, const std::string &title,
                                       const std::string &message, const std::vector<std::string> &buttons, std::int32_t default_button,
                                       std::int32_t escape_button);

    /// `x, y, width, height, cursor_offset_x` in physical pixels.
    void window_request_text_input_area(Engine::Engine &engine, std::uint64_t window, std::array<float, 5> area);
    void window_request_text_input_active(Engine::Engine &engine, std::uint64_t window, bool active);
    /// Drains finished spawn/close/recreate requests. `messages` is cleared and receives each
    /// completion's message at the same index.
    [[nodiscard]] std::unique_ptr<std::vector<WindowRequestCompletion>> window_take_completions(Engine::Engine &engine,
                                                                                                std::vector<std::string> &messages);

} // namespace SFT::CxxApi
