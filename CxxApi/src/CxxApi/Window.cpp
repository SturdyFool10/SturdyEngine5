#include <CxxApi/Window.hpp>

#include <algorithm>
#include <expected>
#include <string>
#include <string_view>

#include <optional>

#include <Engine/WindowControl.hpp>
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
        static_assert(static_cast<int>(WindowEffect::VisualEffect) == static_cast<int>(W::WindowEffectKind::VisualEffect));
        static_assert(static_cast<int>(WindowEffect::GlassEffect) == static_cast<int>(W::WindowEffectKind::GlassEffect));
        static_assert(static_cast<int>(WindowEffect::CornerRadius) == static_cast<int>(W::WindowEffectKind::CornerRadius));
        static_assert(static_cast<int>(LinuxBlurProtocol::KdeX11) == static_cast<int>(W::LinuxBlurProtocol::KdeX11));
        static_assert(static_cast<int>(WindowCornerPreference::RoundSmall) == static_cast<int>(W::WindowCornerPreference::RoundSmall));
        static_assert(static_cast<int>(MacMaterial::UnderPageBackground) == static_cast<int>(W::MacVisualEffectMaterial::UnderPageBackground));
        static_assert(static_cast<int>(MacAppearance::VibrantDark) == static_cast<int>(W::MacAppearance::VibrantDark));
        static_assert(static_cast<int>(MacGlassStyle::Clear) == static_cast<int>(W::MacGlassStyle::Clear));
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

    void window_request_effect_ex(E::Engine &engine, std::uint64_t window, WindowEffect effect, bool enabled,
                                  const WindowEffectOptions &options) {
        W::WindowEffect request{};
        request.kind = static_cast<W::WindowEffectKind>(effect);
        request.enabled = enabled;
        request.linux_blur_protocol = static_cast<W::LinuxBlurProtocol>(options.linux_blur_protocol);
        request.color_argb = options.color_argb;
        W::WindowEffectOptions &out = request.options;
        out.corner_radius = std::max(options.blur_corner_radius, 0.0f);
        out.tint_argb = options.tint_argb;
        out.accent_border_left = options.accent_border_left;
        out.accent_border_top = options.accent_border_top;
        out.accent_border_right = options.accent_border_right;
        out.accent_border_bottom = options.accent_border_bottom;
        out.hide_border = options.hide_border;
        out.corner_preference = static_cast<W::WindowCornerPreference>(options.corner_preference);
        out.mac_material = static_cast<W::MacVisualEffectMaterial>(options.mac_material);
        out.mac_blending = static_cast<W::MacBlendingMode>(options.mac_blending);
        out.mac_state = static_cast<W::MacVisualEffectState>(options.mac_state);
        out.mac_emphasized = options.mac_emphasized;
        out.mac_appearance = static_cast<W::MacAppearance>(options.mac_appearance);
        out.mac_glass_style = static_cast<W::MacGlassStyle>(options.mac_glass_style);
        out.mac_glass_corner_radius = std::max(options.mac_glass_corner_radius, 0.0f);
        out.opacity = std::clamp(options.opacity, 0.0f, 1.0f);
        out.window_corner_radius = std::max(options.window_corner_radius, 0.0f);
        engine.window_requests().set_effect(window_id(window), request);
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

    // ---- the union surface, for any window ----

    namespace {

        namespace C = SFT::Engine::WindowControl;

        static_assert(static_cast<std::uint32_t>(WindowFeature::PenInput) + 1 == static_cast<std::uint32_t>(W::WindowFeature::Count),
                      "the CxxApi feature list must mirror the engine's");
        static_assert(static_cast<std::uint32_t>(WindowOperation::MessageBox) + 1 == static_cast<std::uint32_t>(E::WindowOperation::Count),
                      "the CxxApi operation list must mirror the engine's");

        [[nodiscard]] std::vector<std::byte> bytes_of(const std::vector<std::uint8_t> &source) {
            const auto *data = reinterpret_cast<const std::byte *>(source.data());
            return std::vector<std::byte>(data, data + source.size());
        }

        [[nodiscard]] std::optional<E::WindowOperationStatus> status_of(E::Engine &engine, std::uint64_t window, WindowOperation operation) {
            return engine.window_requests().operation_status(window_id(window), static_cast<E::WindowOperation>(operation));
        }

        [[nodiscard]] std::optional<W::NativeWindowHandle> native_of(E::Engine &engine, std::uint64_t window) {
            const E::WindowSnapshot *snapshot = engine.window_state().find(window_id(window));
            return snapshot != nullptr ? snapshot->native_handle : std::nullopt;
        }

    } // namespace

    bool window_feature_supported(E::Engine &engine, std::uint64_t window, WindowFeature feature) {
        const E::WindowSnapshot *snapshot = engine.window_state().find(window_id(window));
        return snapshot != nullptr && snapshot->feature(static_cast<W::WindowFeature>(feature)).supported;
    }

    std::unique_ptr<std::string> window_feature_note(E::Engine &engine, std::uint64_t window, WindowFeature feature) {
        const E::WindowSnapshot *snapshot = engine.window_state().find(window_id(window));
        return std::make_unique<std::string>(snapshot != nullptr ? std::string{snapshot->feature(static_cast<W::WindowFeature>(feature)).note}
                                                                 : std::string{});
    }

    WindowGeometryInfo window_geometry(E::Engine &engine, std::uint64_t window) {
        WindowGeometryInfo info;
        if (const E::WindowSnapshot *snapshot = engine.window_state().find(window_id(window))) {
            info.frame_left = snapshot->frame_extents.left;
            info.frame_top = snapshot->frame_extents.top;
            info.frame_right = snapshot->frame_extents.right;
            info.frame_bottom = snapshot->frame_extents.bottom;
            info.safe_x = snapshot->safe_area.x;
            info.safe_y = snapshot->safe_area.y;
            info.safe_width = snapshot->safe_area.width;
            info.safe_height = snapshot->safe_area.height;
            info.has_display = snapshot->display_id.has_value();
            info.display_id = snapshot->display_id.value_or(0);
        }
        return info;
    }

    WindowOperationInfo window_operation_status(E::Engine &engine, std::uint64_t window, WindowOperation operation) {
        WindowOperationInfo info;
        if (const auto status = status_of(engine, window, operation)) {
            info.completed = true;
            info.ok = status->ok;
            info.error = status->ok ? 0
                         : status->code == W::WindowErrorCode::Unsupported     ? 1
                         : status->code == W::WindowErrorCode::InvalidArgument ? 2
                                                                               : 3;
            info.flag = status->output.flag;
            info.number = static_cast<std::int32_t>(status->output.number);
        }
        return info;
    }

    std::unique_ptr<std::string> window_operation_message(E::Engine &engine, std::uint64_t window, WindowOperation operation) {
        const auto status = status_of(engine, window, operation);
        return std::make_unique<std::string>(status ? status->message : std::string{});
    }

    std::unique_ptr<std::string> window_operation_text(E::Engine &engine, std::uint64_t window, WindowOperation operation) {
        const auto status = status_of(engine, window, operation);
        return std::make_unique<std::string>(status ? status->output.text : std::string{});
    }

    std::unique_ptr<std::vector<std::uint8_t>> window_operation_bytes(E::Engine &engine, std::uint64_t window, WindowOperation operation) {
        const auto status = status_of(engine, window, operation);
        auto out = std::make_unique<std::vector<std::uint8_t>>();
        if (status) {
            out->reserve(status->output.bytes.size());
            for (const std::byte b : status->output.bytes) {
                out->push_back(static_cast<std::uint8_t>(b));
            }
        }
        return out;
    }

    std::unique_ptr<std::vector<std::string>> window_operation_list(E::Engine &engine, std::uint64_t window, WindowOperation operation) {
        const auto status = status_of(engine, window, operation);
        return std::make_unique<std::vector<std::string>>(status ? status->output.list : std::vector<std::string>{});
    }

    void window_queue_set_title(E::Engine &engine, std::uint64_t window, const std::string &title) {
        C::set_title(engine.window_requests(), window_id(window), title);
    }
    void window_queue_set_position(E::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y) {
        C::set_position(engine.window_requests(), window_id(window), W::WindowPosition{x, y});
    }
    void window_queue_set_size(E::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height) {
        C::set_size(engine.window_requests(), window_id(window), W::WindowExtent{width, height});
    }
    void window_queue_set_minimum_size(E::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height) {
        C::set_minimum_size(engine.window_requests(), window_id(window), W::WindowExtent{width, height});
    }
    void window_queue_set_maximum_size(E::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height) {
        C::set_maximum_size(engine.window_requests(), window_id(window), W::WindowExtent{width, height});
    }
    void window_queue_set_resizable(E::Engine &engine, std::uint64_t window, bool resizable) {
        C::set_resizable(engine.window_requests(), window_id(window), resizable);
    }
    void window_queue_set_opacity(E::Engine &engine, std::uint64_t window, float opacity) {
        C::set_opacity(engine.window_requests(), window_id(window), opacity);
    }
    void window_queue_maximize(E::Engine &engine, std::uint64_t window) { C::maximize(engine.window_requests(), window_id(window)); }
    void window_queue_minimize(E::Engine &engine, std::uint64_t window) { C::minimize(engine.window_requests(), window_id(window)); }
    void window_queue_restore(E::Engine &engine, std::uint64_t window) { C::restore(engine.window_requests(), window_id(window)); }
    void window_queue_raise(E::Engine &engine, std::uint64_t window) { C::raise(engine.window_requests(), window_id(window)); }
    void window_queue_focus(E::Engine &engine, std::uint64_t window) { C::focus(engine.window_requests(), window_id(window)); }
    void window_queue_set_always_on_top(E::Engine &engine, std::uint64_t window, bool enabled) {
        C::set_always_on_top(engine.window_requests(), window_id(window), enabled);
    }
    void window_queue_set_aspect_ratio(E::Engine &engine, std::uint64_t window, float min_ratio, float max_ratio) {
        C::set_aspect_ratio(engine.window_requests(), window_id(window), min_ratio, max_ratio);
    }
    void window_queue_set_mouse_passthrough(E::Engine &engine, std::uint64_t window, bool enabled) {
        C::set_mouse_passthrough(engine.window_requests(), window_id(window), enabled);
    }
    void window_queue_set_focusable(E::Engine &engine, std::uint64_t window, bool enabled) {
        C::set_focusable(engine.window_requests(), window_id(window), enabled);
    }
    void window_queue_set_auto_minimize_on_focus_loss(E::Engine &engine, std::uint64_t window, bool enabled) {
        C::set_auto_minimize_on_focus_loss(engine.window_requests(), window_id(window), enabled);
    }
    void window_queue_set_parent(E::Engine &engine, std::uint64_t window, bool has_parent, std::uint64_t parent, bool modal) {
        std::optional<W::NativeWindowHandle> parent_handle;
        if (has_parent) {
            parent_handle = native_of(engine, parent);
            if (!parent_handle) {
                fail("the parent window does not exist or has no native handle yet");
            }
        }
        C::set_parent(engine.window_requests(), window_id(window), parent_handle, modal);
    }
    void window_queue_set_keyboard_grab(E::Engine &engine, std::uint64_t window, bool enabled) {
        C::set_keyboard_grab(engine.window_requests(), window_id(window), enabled);
    }
    void window_queue_set_mouse_confine_rect(E::Engine &engine, std::uint64_t window, bool has_rect, std::int32_t x, std::int32_t y,
                                             std::int32_t width, std::int32_t height) {
        C::set_mouse_confine_rect(engine.window_requests(), window_id(window),
                                  has_rect ? std::optional<W::WindowRect>{W::WindowRect{x, y, width, height}} : std::nullopt);
    }
    void window_queue_set_cursor_position(E::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y) {
        C::set_cursor_position(engine.window_requests(), window_id(window), W::WindowPosition{x, y});
    }
    void window_queue_set_global_cursor_position(E::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y) {
        C::set_global_cursor_position(engine.window_requests(), window_id(window), W::WindowPosition{x, y});
    }
    void window_queue_set_mouse_capture(E::Engine &engine, std::uint64_t window, bool enabled) {
        C::set_mouse_capture(engine.window_requests(), window_id(window), enabled);
    }
    void window_queue_set_custom_cursor(E::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                                        const std::vector<std::uint8_t> &rgba, std::int32_t hot_x, std::int32_t hot_y) {
        C::set_custom_cursor(engine.window_requests(), window_id(window), width, height, bytes_of(rgba), hot_x, hot_y);
    }
    void window_queue_set_animated_cursor(E::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                                          const std::vector<std::uint8_t> &frames, const std::vector<std::uint32_t> &durations_ms,
                                          std::int32_t hot_x, std::int32_t hot_y) {
        const std::size_t frame_bytes = static_cast<std::size_t>(width) * height * 4;
        if (frame_bytes == 0 || durations_ms.empty() || frames.size() < frame_bytes * durations_ms.size()) {
            fail("an animated cursor needs one frame of pixels per duration");
        }
        std::vector<C::AnimatedCursorFrame> list;
        for (std::size_t i = 0; i < durations_ms.size(); ++i) {
            const std::vector<std::uint8_t> one(frames.begin() + static_cast<std::ptrdiff_t>(i * frame_bytes),
                                                frames.begin() + static_cast<std::ptrdiff_t>((i + 1) * frame_bytes));
            list.push_back({bytes_of(one), durations_ms[i]});
        }
        C::set_animated_cursor(engine.window_requests(), window_id(window), width, height, std::move(list), hot_x, hot_y);
    }
    void window_queue_clear_custom_cursor(E::Engine &engine, std::uint64_t window) {
        C::clear_custom_cursor(engine.window_requests(), window_id(window));
    }
    void window_queue_set_fullscreen_target(E::Engine &engine, std::uint64_t window, bool has_display, std::uint32_t display_id,
                                            std::uint32_t mode_width, std::uint32_t mode_height, float refresh_hz) {
        W::FullscreenTarget target;
        if (has_display) {
            target.display_id = display_id;
        }
        if (mode_width != 0 && mode_height != 0) {
            target.mode = W::DisplayMode{mode_width, mode_height, refresh_hz, 1.0f};
        }
        C::set_fullscreen_target(engine.window_requests(), window_id(window), target);
    }
    void window_queue_set_window_type(E::Engine &engine, std::uint64_t window, WindowType window_type) {
        C::set_window_type(engine.window_requests(), window_id(window), static_cast<W::WindowType>(window_type));
    }
    void window_queue_set_shape(E::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                                const std::vector<std::uint8_t> &coverage) {
        C::set_shape(engine.window_requests(), window_id(window), width, height, bytes_of(coverage));
    }
    void window_queue_set_icon(E::Engine &engine, std::uint64_t window, std::uint32_t width, std::uint32_t height,
                               const std::vector<std::uint8_t> &rgba) {
        C::set_icon(engine.window_requests(), window_id(window), width, height, bytes_of(rgba));
    }
    void window_queue_request_attention(E::Engine &engine, std::uint64_t window, std::uint8_t request) {
        C::request_attention(engine.window_requests(), window_id(window),
                             request == 0 ? W::AttentionRequest::Cancel : request == 1 ? W::AttentionRequest::Briefly : W::AttentionRequest::UntilFocused);
    }
    void window_queue_set_taskbar_progress(E::Engine &engine, std::uint64_t window, std::uint8_t state, float value) {
        C::set_taskbar_progress(engine.window_requests(), window_id(window), static_cast<W::TaskbarProgress>(std::min<std::uint8_t>(state, 4)), value);
    }
    void window_queue_show_system_menu(E::Engine &engine, std::uint64_t window, std::int32_t x, std::int32_t y) {
        C::show_system_menu(engine.window_requests(), window_id(window), W::WindowPosition{x, y});
    }
    void window_queue_set_display_gamma(E::Engine &engine, std::uint64_t window, bool has_display, std::uint32_t display_id, float gamma) {
        C::set_display_gamma(engine.window_requests(), window_id(window), has_display ? std::optional<std::uint32_t>{display_id} : std::nullopt, gamma);
    }
    void window_queue_query_clipboard_text(E::Engine &engine, std::uint64_t window) { C::clipboard_text(engine.window_requests(), window_id(window)); }
    void window_queue_set_clipboard_text(E::Engine &engine, std::uint64_t window, const std::string &text) {
        C::set_clipboard_text(engine.window_requests(), window_id(window), text);
    }
    void window_queue_query_clipboard_mime_types(E::Engine &engine, std::uint64_t window) {
        C::clipboard_mime_types(engine.window_requests(), window_id(window));
    }
    void window_queue_query_key_name(E::Engine &engine, std::uint64_t window, std::int32_t key) {
        C::key_name(engine.window_requests(), window_id(window), static_cast<W::KeyboardKey>(key));
    }
    void window_queue_query_key_down(E::Engine &engine, std::uint64_t window, std::int32_t key) {
        C::key_down(engine.window_requests(), window_id(window), static_cast<W::KeyboardKey>(key));
    }
    void window_queue_query_icc_profile(E::Engine &engine, std::uint64_t window) { C::icc_profile(engine.window_requests(), window_id(window)); }

    void window_queue_show_message_box(E::Engine &engine, std::uint64_t window, std::uint8_t kind, const std::string &title,
                                       const std::string &message, const std::vector<std::string> &buttons, std::int32_t default_button,
                                       std::int32_t escape_button) {
        W::Desktop::MessageBoxOptions options;
        options.kind = static_cast<W::Desktop::MessageBoxKind>(std::min<std::uint8_t>(kind, 2));
        options.title = title;
        options.message = message;
        options.buttons = buttons;
        options.default_button = default_button;
        options.escape_button = escape_button;
        C::show_message_box(engine.window_requests(), window_id(window), std::move(options));
    }

} // namespace SFT::CxxApi
