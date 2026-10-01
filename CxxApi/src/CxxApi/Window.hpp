#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <Engine/Engine.hpp>

#include <CxxApi/Types/Window.hpp>

namespace SFT::CxxApi {

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
    /// `x, y, width, height, cursor_offset_x` in physical pixels.
    void window_request_text_input_area(Engine::Engine &engine, std::uint64_t window, std::array<float, 5> area);
    void window_request_text_input_active(Engine::Engine &engine, std::uint64_t window, bool active);
    /// Drains finished spawn/close/recreate requests. `messages` is cleared and receives each
    /// completion's message at the same index.
    [[nodiscard]] std::unique_ptr<std::vector<WindowRequestCompletion>> window_take_completions(Engine::Engine &engine,
                                                                                                std::vector<std::string> &messages);

} // namespace SFT::CxxApi
