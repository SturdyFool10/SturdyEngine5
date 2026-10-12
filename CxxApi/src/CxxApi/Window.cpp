#include <CxxApi/Window.hpp>

#include <expected>
#include <string>
#include <string_view>

#include <Engine/WindowRequests.hpp>
#include <Engine/WindowState.hpp>
#include <RHI/RHI.hpp>
#include <WindowManager/WindowManager.hpp>

#include <CxxApi/Conversions.hpp>
#include <CxxApi/Error.hpp>

namespace SFT::CxxApi {

    namespace {
        namespace E = SFT::Engine;
        namespace W = SFT::WindowManager;

        // The CxxApi enums mirror the engine's numerically so a conversion is a cast; catch drift at compile time.
        static_assert(static_cast<int>(WindowMode::ExclusiveFullscreen) == static_cast<int>(W::WindowMode::ExclusiveFullscreen));
        static_assert(static_cast<int>(CursorIcon::NotAllowed) == static_cast<int>(W::CursorIcon::NotAllowed));
        static_assert(static_cast<int>(CursorIcon::ResizeNesw) == static_cast<int>(W::CursorIcon::ResizeNesw));
        static_assert(static_cast<int>(WindowEffect::Transparent) == static_cast<int>(W::WindowEffectKind::Transparent));
        static_assert(static_cast<int>(WindowEffect::MicaAlt) == static_cast<int>(W::WindowEffectKind::MicaAlt));
        static_assert(static_cast<int>(WindowRequestKind::RecreatePrimary) == static_cast<int>(E::WindowRequestKind::RecreatePrimary));
        static_assert(static_cast<int>(WindowRequestKind::Close) == static_cast<int>(E::WindowRequestKind::Close));

        [[nodiscard]] W::WindowId window_id(std::uint64_t id) noexcept { return static_cast<W::WindowId>(id); }
        [[nodiscard]] W::WindowMode to_mode(WindowMode mode) noexcept { return static_cast<W::WindowMode>(mode); }

        /// The primary window, or throws `Error` when the engine has none.
        [[nodiscard]] W::Window &primary(E::Engine &engine) {
            W::Window *window = engine.primary_window();
            if (window == nullptr) {
                fail("the engine has no primary window");
            }
            return *window;
        }

        /// Throws `Error` carrying the platform's message when `result` holds an error.
        template <typename T> T unwrap(std::expected<T, W::WindowError> result) {
            if (!result) {
                fail(result.error().message);
            }
            if constexpr (!std::is_void_v<T>) {
                return std::move(*result);
            }
        }

        /// Calls `call` on the primary window and throws on a missing window or a platform refusal.
        template <typename F> void with_primary(E::Engine &engine, F &&call) { unwrap(call(primary(engine))); }

        [[nodiscard]] Int2Result to_int2(const std::expected<W::WindowExtent, W::WindowError> &r) noexcept {
            return r ? Int2Result{true, static_cast<std::int32_t>(r->x), static_cast<std::int32_t>(r->y)} : Int2Result{};
        }
        [[nodiscard]] Int2Result to_int2(const std::expected<W::WindowPosition, W::WindowError> &r) noexcept {
            return r ? Int2Result{true, r->x, r->y} : Int2Result{};
        }

        [[nodiscard]] WindowSnapshot to_snapshot(const E::WindowSnapshot &s) noexcept {
            WindowSnapshot out{};
            out.id = static_cast<std::uint64_t>(s.id);
            out.width = s.size.x;
            out.height = s.size.y;
            out.framebuffer_width = s.framebuffer_size.x;
            out.framebuffer_height = s.framebuffer_size.y;
            out.position_x = s.position.x;
            out.position_y = s.position.y;
            out.content_scale = s.content_scale;
            out.opacity = s.opacity;
            out.mouse_locked = s.mouse_locked;
            out.focused = s.focused;
            out.primary = false;
            return out;
        }

        /// A new window's engine configuration. The title pointer is only valid for the caller's `title`, which the request
        /// queue copies before returning. The window API always matches the active backend.
        [[nodiscard]] W::WindowConfig to_config(E::Engine &engine, const WindowDesc &desc, const std::string &title) {
            RHI::RhiDevice *device = engine.rhi_device();
            if (device == nullptr) {
                fail("the engine has no active graphics device");
            }
            W::WindowConfig config{};
            switch (device->backend_type()) {
                case RHI::BackendType::Vulkan: config.graphics_api = W::WindowGraphicsApi::Vulkan; break;
                case RHI::BackendType::D3D12: config.graphics_api = W::WindowGraphicsApi::Direct3D; break;
                case RHI::BackendType::Metal: config.graphics_api = W::WindowGraphicsApi::Metal; break;
                case RHI::BackendType::WebGpu: config.graphics_api = W::WindowGraphicsApi::WebGPU; break;
                default: fail("the active graphics backend has no window-creation counterpart");
            }
            config.title = title.c_str();
            config.extent = W::WindowExtent{desc.width, desc.height};
            config.position = W::WindowPosition{desc.x, desc.y};
            config.use_default_position = !desc.has_position;
            config.visible = desc.visible;
            config.resizable = desc.resizable;
            config.decorated = desc.decorated;
            config.high_dpi = desc.high_dpi;
            config.transparent = desc.transparent;
            config.mode = to_mode(desc.mode);
            return config;
        }
    } // namespace

    // ---- the primary window ----

    WindowDesc window_desc_defaults() noexcept {
        const W::WindowConfig defaults{};
        WindowDesc desc{};
        desc.width = defaults.extent.x;
        desc.height = defaults.extent.y;
        desc.has_position = !defaults.use_default_position;
        desc.x = defaults.position.x;
        desc.y = defaults.position.y;
        desc.visible = defaults.visible;
        desc.resizable = defaults.resizable;
        desc.decorated = defaults.decorated;
        desc.high_dpi = defaults.high_dpi;
        desc.transparent = defaults.transparent;
        desc.mode = WindowMode::Windowed;
        return desc;
    }

    bool window_has_primary(E::Engine &engine) noexcept { return engine.primary_window() != nullptr; }

    bool window_close_requested(E::Engine &engine) noexcept {
        const W::Window *window = engine.primary_window();
        return window != nullptr && window->close_requested();
    }

    void window_request_close(E::Engine &engine) noexcept {
        if (W::Window *window = engine.primary_window()) {
            window->request_close();
        }
    }

    void window_show(E::Engine &engine) { with_primary(engine, [](W::Window &w) { return w.show(); }); }
    void window_hide(E::Engine &engine) { with_primary(engine, [](W::Window &w) { return w.hide(); }); }
    void window_focus(E::Engine &engine) { with_primary(engine, [](W::Window &w) { return w.focus(); }); }
    void window_raise(E::Engine &engine) { with_primary(engine, [](W::Window &w) { return w.raise(); }); }
    void window_maximize(E::Engine &engine) { with_primary(engine, [](W::Window &w) { return w.maximize(); }); }
    void window_minimize(E::Engine &engine) { with_primary(engine, [](W::Window &w) { return w.minimize(); }); }
    void window_restore(E::Engine &engine) { with_primary(engine, [](W::Window &w) { return w.restore(); }); }

    void window_set_title(E::Engine &engine, const std::string &title) {
        with_primary(engine, [&](W::Window &w) { return w.set_title(title.c_str()); });
    }

    Int2Result window_position(E::Engine &engine) {
        const W::Window *window = engine.primary_window();
        return window != nullptr ? to_int2(window->position()) : Int2Result{};
    }

    void window_set_position(E::Engine &engine, std::int32_t x, std::int32_t y) {
        with_primary(engine, [&](W::Window &w) { return w.set_position(W::WindowPosition{x, y}); });
    }

    Int2Result window_global_cursor_position(E::Engine &engine) {
        const W::Window *window = engine.primary_window();
        return window != nullptr ? to_int2(window->global_cursor_position()) : Int2Result{};
    }

    Int2Result window_size(E::Engine &engine) {
        const W::Window *window = engine.primary_window();
        return window != nullptr ? to_int2(window->size()) : Int2Result{};
    }

    Int2Result window_framebuffer_size(E::Engine &engine) {
        const W::Window *window = engine.primary_window();
        return window != nullptr ? to_int2(window->framebuffer_size()) : Int2Result{};
    }

    void window_set_size(E::Engine &engine, std::uint32_t width, std::uint32_t height) {
        with_primary(engine, [&](W::Window &w) { return w.set_size(W::WindowExtent{width, height}); });
    }

    void window_set_minimum_size(E::Engine &engine, std::uint32_t width, std::uint32_t height) {
        with_primary(engine, [&](W::Window &w) { return w.set_minimum_size(W::WindowExtent{width, height}); });
    }

    void window_set_maximum_size(E::Engine &engine, std::uint32_t width, std::uint32_t height) {
        with_primary(engine, [&](W::Window &w) { return w.set_maximum_size(W::WindowExtent{width, height}); });
    }

    void window_set_resizable(E::Engine &engine, bool resizable) {
        with_primary(engine, [&](W::Window &w) { return w.set_resizable(resizable); });
    }

    WindowMode window_fullscreen_mode(E::Engine &engine) noexcept {
        const W::Window *window = engine.primary_window();
        return window != nullptr ? static_cast<WindowMode>(window->fullscreen_mode()) : WindowMode::Windowed;
    }

    void window_set_opacity(E::Engine &engine, float opacity) {
        with_primary(engine, [&](W::Window &w) { return w.set_opacity(opacity); });
    }

    float window_opacity(E::Engine &engine) { return unwrap(primary(engine).opacity()); }

    void window_set_cursor_visible(E::Engine &engine, bool visible) {
        with_primary(engine, [&](W::Window &w) { return w.set_cursor_visible(visible); });
    }

    float window_content_scale(E::Engine &engine) noexcept {
        const W::Window *window = engine.primary_window();
        return window != nullptr ? window->content_scale() : 1.0f;
    }

    float window_refresh_rate_hz(E::Engine &engine) { return unwrap(primary(engine).refresh_rate_hz()); }

    std::unique_ptr<std::string> window_clipboard_text(E::Engine &engine) { return owned_string(primary(engine).clipboard_text()); }

    void window_set_clipboard_text(E::Engine &engine, const std::string &text) {
        with_primary(engine, [&](W::Window &w) { return w.set_clipboard_text(text); });
    }

    HdrProperties window_hdr_properties(E::Engine &engine) {
        HdrProperties out{};
        if (const W::Window *window = engine.primary_window()) {
            if (const auto props = window->hdr_properties()) {
                out.available = true;
                out.hdr_enabled = props->hdr_enabled;
                out.sdr_white_level = props->sdr_white_level;
                out.hdr_headroom = props->hdr_headroom;
            }
        }
        return out;
    }

    // ---- every managed window ----

    std::size_t window_count(const E::Engine &engine) noexcept { return engine.window_state().windows().size(); }

    WindowSnapshot window_snapshot_at(const E::Engine &engine, std::size_t index) {
        const auto windows = engine.window_state().windows();
        if (index >= windows.size()) {
            fail("window index is out of range");
        }
        WindowSnapshot out = to_snapshot(windows[index]);
        const E::WindowSnapshot *primary_snapshot = engine.window_state().primary();
        out.primary = primary_snapshot != nullptr && primary_snapshot->id == windows[index].id;
        return out;
    }

    bool window_find(const E::Engine &engine, std::uint64_t window, WindowSnapshot &out) noexcept {
        const E::WindowSnapshot *found = engine.window_state().find(window_id(window));
        if (found == nullptr) {
            return false;
        }
        out = to_snapshot(*found);
        const E::WindowSnapshot *primary_snapshot = engine.window_state().primary();
        out.primary = primary_snapshot != nullptr && primary_snapshot->id == found->id;
        return true;
    }

    bool window_primary_snapshot(const E::Engine &engine, WindowSnapshot &out) noexcept {
        const E::WindowSnapshot *found = engine.window_state().primary();
        if (found == nullptr) {
            return false;
        }
        out = to_snapshot(*found);
        out.primary = true;
        return true;
    }

    // ---- queued requests ----

    std::uint64_t window_request_spawn(E::Engine &engine, const WindowDesc &desc, const std::string &title) {
        return engine.window_requests().spawn(to_config(engine, desc, title)).value;
    }

    std::uint64_t window_request_recreate_primary(E::Engine &engine, const WindowDesc &desc, const std::string &title) {
        return engine.window_requests().recreate_primary_window(to_config(engine, desc, title)).value;
    }

    std::uint64_t window_request_close_window(E::Engine &engine, std::uint64_t window) {
        return engine.window_requests().close(window_id(window)).value;
    }

    void window_request_cursor_icon(E::Engine &engine, std::uint64_t window, CursorIcon icon) {
        engine.window_requests().set_cursor_icon(window_id(window), static_cast<W::CursorIcon>(icon));
    }

    void window_request_fullscreen(E::Engine &engine, std::uint64_t window, WindowMode mode) {
        engine.window_requests().set_fullscreen(window_id(window), to_mode(mode));
    }

    void window_request_decorated(E::Engine &engine, std::uint64_t window, bool decorated) {
        engine.window_requests().set_decorated(window_id(window), decorated);
    }

    void window_request_transparent(E::Engine &engine, std::uint64_t window, bool transparent) {
        engine.window_requests().set_transparent(window_id(window), transparent);
    }

    void window_request_relative_mouse_mode(E::Engine &engine, std::uint64_t window, bool enabled) {
        engine.window_requests().set_relative_mouse_mode(window_id(window), enabled);
    }

    void window_request_mouse_locked(E::Engine &engine, std::uint64_t window, bool locked) {
        engine.window_requests().set_mouse_locked(window_id(window), locked);
    }

    void window_request_cursor_grabbed(E::Engine &engine, std::uint64_t window, bool grabbed) {
        engine.window_requests().set_cursor_grabbed(window_id(window), grabbed);
    }

    void window_request_effect(E::Engine &engine, std::uint64_t window, WindowEffect effect, bool enabled) {
        engine.window_requests().set_blur(window_id(window), static_cast<W::WindowEffectKind>(effect), enabled);
    }

    void window_request_text_input_area(E::Engine &engine, std::uint64_t window, std::array<float, 5> area) {
        engine.window_requests().set_text_input_area(window_id(window), W::TextInputArea{area[0], area[1], area[2], area[3], area[4]});
    }

    void window_request_text_input_active(E::Engine &engine, std::uint64_t window, bool active) {
        engine.window_requests().set_text_input_active(window_id(window), active);
    }

    std::unique_ptr<std::vector<WindowRequestCompletion>> window_take_completions(E::Engine &engine, std::vector<std::string> &messages) {
        const std::vector<E::WindowRequestCompletion> completions = engine.window_requests().take_completions();
        auto out = std::make_unique<std::vector<WindowRequestCompletion>>();
        out->reserve(completions.size());
        messages.clear();
        messages.reserve(completions.size());
        for (const E::WindowRequestCompletion &completion : completions) {
            // A successful spawn/recreate reports the new window through `surface`; a close only sets `window`.
            const W::WindowId affected = completion.surface.has_value() ? completion.surface->window_id : completion.window;
            out->push_back(WindowRequestCompletion{
                .request_id = completion.id.value,
                .kind = static_cast<WindowRequestKind>(completion.kind),
                .accepted = completion.accepted,
                .window_id = static_cast<std::uint64_t>(affected),
            });
            messages.emplace_back(std::string_view{completion.message});
        }
        return out;
    }

} // namespace SFT::CxxApi
