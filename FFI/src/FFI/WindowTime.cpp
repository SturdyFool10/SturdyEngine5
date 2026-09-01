/// C ABI implementation of the window-state, frame-time, and window-request surfaces.
///
/// These are the pieces an application needs every frame that are not rendering: how big the
/// window is, whether it has focus, how much time passed, and how to ask for a window change.
///
/// Window changes are *requests* rather than direct mutations, matching the engine's own
/// `WindowRequests` queue. That is deliberate and load-bearing across this boundary: a foreign
/// caller is most likely to invoke these from inside a render callback, and applying them
/// immediately could destroy or resize the window being drawn. Queuing lets the engine apply them
/// at the point in the frame where that is safe.

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include <Engine/Engine.hpp>
#include <RHI/RHI.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    /// Translates an ABI cursor icon to the engine's own enumeration.
    ///
    /// @param icon Value received from the caller.
    /// @param out_icon Receives the translated value.
    ///
    /// @return Returns `true` when `icon` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_cursor_icon(SturdyCursorIcon icon,
                                             SFT::WindowManager::CursorIcon *out_icon) noexcept {
        switch (icon) {
        case STURDY_CURSOR_ICON_DEFAULT:
            *out_icon = SFT::WindowManager::CursorIcon::Default;
            return true;
        case STURDY_CURSOR_ICON_POINTER:
            *out_icon = SFT::WindowManager::CursorIcon::Pointer;
            return true;
        case STURDY_CURSOR_ICON_TEXT:
            *out_icon = SFT::WindowManager::CursorIcon::Text;
            return true;
        case STURDY_CURSOR_ICON_GRAB:
            *out_icon = SFT::WindowManager::CursorIcon::Grab;
            return true;
        case STURDY_CURSOR_ICON_GRABBING:
            *out_icon = SFT::WindowManager::CursorIcon::Grabbing;
            return true;
        case STURDY_CURSOR_ICON_RESIZE_HORIZONTAL:
            *out_icon = SFT::WindowManager::CursorIcon::ResizeHorizontal;
            return true;
        case STURDY_CURSOR_ICON_RESIZE_VERTICAL:
            *out_icon = SFT::WindowManager::CursorIcon::ResizeVertical;
            return true;
        case STURDY_CURSOR_ICON_RESIZE_NWSE:
            *out_icon = SFT::WindowManager::CursorIcon::ResizeNwse;
            return true;
        case STURDY_CURSOR_ICON_RESIZE_NESW:
            *out_icon = SFT::WindowManager::CursorIcon::ResizeNesw;
            return true;
        case STURDY_CURSOR_ICON_NOT_ALLOWED:
            *out_icon = SFT::WindowManager::CursorIcon::NotAllowed;
            return true;
        case STURDY_CURSOR_ICON_FORCE_U32:
        default:
            return false;
        }
    }

    /// Translates an ABI window effect kind to the engine's own enumeration.
    ///
    /// @param kind Value received from the caller.
    /// @param out_kind Receives the translated value.
    ///
    /// @return Returns `true` when `kind` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_window_effect_kind(SturdyWindowEffectKind kind,
                                                    SFT::WindowManager::WindowEffectKind *out_kind) noexcept {
        switch (kind) {
        case STURDY_WINDOW_EFFECT_BLUR:
            *out_kind = SFT::WindowManager::WindowEffectKind::Blur;
            return true;
        case STURDY_WINDOW_EFFECT_ACRYLIC:
            *out_kind = SFT::WindowManager::WindowEffectKind::Acrylic;
            return true;
        case STURDY_WINDOW_EFFECT_MICA:
            *out_kind = SFT::WindowManager::WindowEffectKind::Mica;
            return true;
        case STURDY_WINDOW_EFFECT_MICA_ALT:
            *out_kind = SFT::WindowManager::WindowEffectKind::MicaAlt;
            return true;
        case STURDY_WINDOW_EFFECT_TABBED:
            *out_kind = SFT::WindowManager::WindowEffectKind::Tabbed;
            return true;
        case STURDY_WINDOW_EFFECT_DARK_MODE:
            *out_kind = SFT::WindowManager::WindowEffectKind::DarkMode;
            return true;
        case STURDY_WINDOW_EFFECT_BORDER_COLOR:
            *out_kind = SFT::WindowManager::WindowEffectKind::BorderColor;
            return true;
        case STURDY_WINDOW_EFFECT_CAPTION_COLOR:
            *out_kind = SFT::WindowManager::WindowEffectKind::CaptionColor;
            return true;
        case STURDY_WINDOW_EFFECT_TEXT_COLOR:
            *out_kind = SFT::WindowManager::WindowEffectKind::TextColor;
            return true;
        case STURDY_WINDOW_EFFECT_TRANSPARENT:
            *out_kind = SFT::WindowManager::WindowEffectKind::Transparent;
            return true;
        case STURDY_WINDOW_EFFECT_FORCE_U32:
        default:
            return false;
        }
    }

    /// Translates an ABI window mode to the engine's own enumeration.
    ///
    /// @param mode Value received from the caller.
    /// @param out_mode Receives the translated value.
    ///
    /// @return Returns `true` when `mode` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_window_mode(SturdyWindowMode mode,
                                             SFT::WindowManager::WindowMode *out_mode) noexcept {
        switch (mode) {
        case STURDY_WINDOW_MODE_WINDOWED:
            *out_mode = SFT::WindowManager::WindowMode::Windowed;
            return true;
        case STURDY_WINDOW_MODE_BORDERLESS_FULLSCREEN:
            *out_mode = SFT::WindowManager::WindowMode::BorderlessFullscreen;
            return true;
        case STURDY_WINDOW_MODE_EXCLUSIVE_FULLSCREEN:
            *out_mode = SFT::WindowManager::WindowMode::ExclusiveFullscreen;
            return true;
        case STURDY_WINDOW_MODE_FORCE_U32:
        default:
            return false;
        }
    }

    /// Resolves an engine handle to its active RHI device.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_device Receives the borrowed device on success.
    ///
    /// @return `STURDY_OK`; `STURDY_ERROR_NOT_AVAILABLE` when the engine has no device yet.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult resolve_device(SturdyEngine engine, SFT::RHI::RhiDevice **out_device) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::RHI::RhiDevice *device = resolved_engine->rhi_device();
        if (device == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the engine has no active RHI device");
        }
        *out_device = device;
        return STURDY_OK;
    }

    /// Maps the engine's active RHI backend to the window graphics API a new window's OS handle
    /// must be created with, so its native surface later accepts that backend's swapchain.
    ///
    /// @param backend Engine-side value.
    /// @param out_api Receives the translated value.
    ///
    /// @return Returns `true` when `backend` has a window-creation counterpart; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_graphics_api(SFT::RHI::BackendType backend,
                                              SFT::WindowManager::WindowGraphicsApi *out_api) noexcept {
        switch (backend) {
        case SFT::RHI::BackendType::Vulkan:
            *out_api = SFT::WindowManager::WindowGraphicsApi::Vulkan;
            return true;
        case SFT::RHI::BackendType::D3D12:
            *out_api = SFT::WindowManager::WindowGraphicsApi::Direct3D;
            return true;
        case SFT::RHI::BackendType::Metal:
            *out_api = SFT::WindowManager::WindowGraphicsApi::Metal;
            return true;
        case SFT::RHI::BackendType::WebGpu:
            *out_api = SFT::WindowManager::WindowGraphicsApi::WebGPU;
            return true;
        default:
            return false;
        }
    }

    /// Translates the engine's window request kind to the ABI's.
    ///
    /// @param kind Engine-side value.
    /// @param out_kind Receives the translated value.
    ///
    /// @return Returns `true` when `kind` has an ABI spelling; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_request_kind(SFT::Engine::WindowRequestKind kind,
                                              SturdyWindowRequestKind *out_kind) noexcept {
        switch (kind) {
        case SFT::Engine::WindowRequestKind::Spawn:
            *out_kind = STURDY_WINDOW_REQUEST_SPAWN;
            return true;
        case SFT::Engine::WindowRequestKind::RecreatePrimary:
            *out_kind = STURDY_WINDOW_REQUEST_RECREATE_PRIMARY;
            return true;
        case SFT::Engine::WindowRequestKind::Close:
            *out_kind = STURDY_WINDOW_REQUEST_CLOSE;
            return true;
        }
        return false;
    }

    /// Reads the window a completion refers to, whichever of its two fields carries it: a
    /// successful spawn/recreate reports it through `surface`, while a close completion (which
    /// never allocates a new surface) only ever sets `window`.
    ///
    /// @param completion Engine-side completion record.
    ///
    /// @return Returns the surface identifier as this ABI spells it.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdySurface completion_surface(const SFT::Engine::WindowRequestCompletion &completion) noexcept {
        const SFT::WindowManager::WindowId window_id =
            completion.surface.has_value() ? completion.surface->window_id : completion.window;
        return SturdySurface{static_cast<uint64_t>(window_id)};
    }

    /// Validates and translates an ABI window config into the engine's own representation.
    ///
    /// @param config Value received from the caller.
    /// @param graphics_api Window graphics API to stamp onto the result, matching the engine's
    ///        active RHI backend.
    /// @param out_config Receives the translated value.
    ///
    /// @return `STURDY_OK`, or the argument failure encountered.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SturdyResult build_window_config(const SturdyWindowConfig *config,
                                                   SFT::WindowManager::WindowGraphicsApi graphics_api,
                                                   SFT::WindowManager::WindowConfig *out_config) noexcept {
        if (config == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "config must not be null");
        }
        if (config->struct_size != sizeof(SturdyWindowConfig)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyWindowConfig size does not match this engine build");
        }
        SFT::WindowManager::WindowMode engine_mode{};
        if (!translate_window_mode(config->mode, &engine_mode)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized window mode");
        }

        // Copied into an owned string before this call returns (WindowRequests::spawn stores an
        // OwnedWindowConfig), so the caller's pointer need not outlive it.
        out_config->title = config->title != nullptr ? config->title : "Sturdy Engine";
        out_config->extent = SFT::WindowManager::WindowExtent{config->width, config->height};
        out_config->position = SFT::WindowManager::WindowPosition{config->position_x, config->position_y};
        out_config->use_default_position = config->use_default_position != STURDY_FALSE;
        out_config->visible = config->visible != STURDY_FALSE;
        out_config->resizable = config->resizable != STURDY_FALSE;
        out_config->decorated = config->decorated != STURDY_FALSE;
        out_config->high_dpi = config->high_dpi != STURDY_FALSE;
        out_config->transparent = config->transparent != STURDY_FALSE;
        out_config->mode = engine_mode;
        out_config->graphics_api = graphics_api;
        return STURDY_OK;
    }

    /// Copies an engine window snapshot into its ABI representation.
    ///
    /// @param snapshot Engine-side snapshot.
    /// @param out_snapshot Destination.
    ///
    /// @note This function does not throw exceptions.
    void copy_snapshot(const SFT::Engine::WindowSnapshot &snapshot,
                       SturdyWindowSnapshot *out_snapshot) noexcept {
        *out_snapshot = SturdyWindowSnapshot{};
        out_snapshot->struct_size = static_cast<uint32_t>(sizeof(SturdyWindowSnapshot));
        out_snapshot->surface_id = static_cast<uint64_t>(snapshot.id);
        out_snapshot->width = snapshot.size.x;
        out_snapshot->height = snapshot.size.y;
        out_snapshot->framebuffer_width = snapshot.framebuffer_size.x;
        out_snapshot->framebuffer_height = snapshot.framebuffer_size.y;
        out_snapshot->position_x = snapshot.position.x;
        out_snapshot->position_y = snapshot.position.y;
        out_snapshot->opacity = snapshot.opacity;
        out_snapshot->mouse_locked = snapshot.mouse_locked ? STURDY_TRUE : STURDY_FALSE;
        out_snapshot->focused = snapshot.focused ? STURDY_TRUE : STURDY_FALSE;
    }

    /// Shared body for the read-only engine queries, which differ only in what they read.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param out_value Destination; must not be null.
    /// @param read Invoked with the resolved engine, returning the value to store.
    ///
    /// @return `STURDY_OK`, or the handle/argument failure encountered.
    /// @note This function does not throw exceptions.
    template <typename Value, typename Read>
    [[nodiscard]] SturdyResult read_engine_value(SturdyEngine engine, Value *out_value, Read read) noexcept {
        if (out_value == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        *out_value = read(*resolved_engine);
        return STURDY_OK;
    }

    /// Shared body for the window-request entry points, which differ only in what they queue.
    ///
    /// @param engine Handle supplied to a game-logic callback.
    /// @param surface Surface identifying the target window.
    /// @param apply Invoked with the engine's request queue and the target window id.
    ///
    /// @return `STURDY_OK`, or the handle failure encountered.
    /// @note This function does not throw exceptions.
    template <typename Apply>
    [[nodiscard]] SturdyResult queue_window_request(SturdyEngine engine,
                                                    SturdySurface surface,
                                                    Apply apply) noexcept {
        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto window_id = static_cast<SFT::WindowManager::WindowId>(surface.id);
        apply(resolved_engine->window_requests(), window_id);
        return STURDY_OK;
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_time_delta_seconds(SturdyEngine engine, double *out_seconds) {
    return guarded([&]() -> SturdyResult {
        return read_engine_value(engine, out_seconds, [](SFT::Engine::Engine &resolved) {
            return resolved.frame_time().delta_seconds();
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_time_unscaled_delta_seconds(SturdyEngine engine, double *out_seconds) {
    return guarded([&]() -> SturdyResult {
        return read_engine_value(engine, out_seconds, [](SFT::Engine::Engine &resolved) {
            return resolved.frame_time().unscaled_delta_seconds();
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_time_tick_index(SturdyEngine engine, uint64_t *out_tick_index) {
    return guarded([&]() -> SturdyResult {
        return read_engine_value(engine, out_tick_index, [](SFT::Engine::Engine &resolved) {
            return resolved.frame_time().tick_index();
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_time_scale(SturdyEngine engine, double *out_scale) {
    return guarded([&]() -> SturdyResult {
        return read_engine_value(engine, out_scale, [](SFT::Engine::Engine &resolved) {
            return resolved.time_scale().value();
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_time_set_scale(SturdyEngine engine, double scale) {
    return guarded([&]() -> SturdyResult {
        // Rejected rather than clamped: a negative or NaN scale is a caller bug, and silently
        // substituting a value would hide it behind a simulation that merely looks wrong.
        if (!std::isfinite(scale) || scale < 0.0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "time scale must be finite and not negative");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        resolved_engine->time_scale().set(scale);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        return read_engine_value(engine, out_count, [](SFT::Engine::Engine &resolved) {
            return static_cast<uint32_t>(resolved.window_state().windows().size());
        });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_snapshot(SturdyEngine engine,
                                                    uint32_t index,
                                                    SturdyWindowSnapshot *out_snapshot) {
    return guarded([&]() -> SturdyResult {
        if (out_snapshot == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto windows = resolved_engine->window_state().windows();
        if (index >= windows.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "window index is out of range");
        }
        copy_snapshot(windows[index], out_snapshot);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_primary(SturdyEngine engine,
                                                   SturdyWindowSnapshot *out_snapshot) {
    return guarded([&]() -> SturdyResult {
        if (out_snapshot == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Engine::WindowSnapshot *primary = resolved_engine->window_state().primary();
        if (primary == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "the engine has no primary window");
        }
        copy_snapshot(*primary, out_snapshot);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_find(SturdyEngine engine,
                                                SturdySurface surface,
                                                SturdyWindowSnapshot *out_snapshot) {
    return guarded([&]() -> SturdyResult {
        if (out_snapshot == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const auto window_id = static_cast<SFT::WindowManager::WindowId>(surface.id);
        const SFT::Engine::WindowSnapshot *found = resolved_engine->window_state().find(window_id);
        if (found == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no managed window matches that surface");
        }
        copy_snapshot(*found, out_snapshot);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_config_init(SturdyWindowConfig *config) {
    return guarded([&]() -> SturdyResult {
        if (config == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const SFT::WindowManager::WindowConfig defaults{};
        *config = SturdyWindowConfig{};
        config->struct_size = static_cast<uint32_t>(sizeof(SturdyWindowConfig));
        config->title = nullptr;
        config->width = defaults.extent.x;
        config->height = defaults.extent.y;
        config->position_x = defaults.position.x;
        config->position_y = defaults.position.y;
        config->use_default_position = defaults.use_default_position ? STURDY_TRUE : STURDY_FALSE;
        config->visible = defaults.visible ? STURDY_TRUE : STURDY_FALSE;
        config->resizable = defaults.resizable ? STURDY_TRUE : STURDY_FALSE;
        config->decorated = defaults.decorated ? STURDY_TRUE : STURDY_FALSE;
        config->high_dpi = defaults.high_dpi ? STURDY_TRUE : STURDY_FALSE;
        config->transparent = defaults.transparent ? STURDY_TRUE : STURDY_FALSE;
        config->mode = STURDY_WINDOW_MODE_WINDOWED;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_spawn(SturdyEngine engine,
                                                 const SturdyWindowConfig *config,
                                                 uint64_t *out_request_id) {
    return guarded([&]() -> SturdyResult {
        // Validated before any handle is consulted, matching every other entry point: a bad
        // argument is a caller bug regardless of engine state.
        SFT::WindowManager::WindowConfig engine_config{};
        const SturdyResult built =
            build_window_config(config, SFT::WindowManager::WindowGraphicsApi::None, &engine_config);
        if (built != STURDY_OK) {
            return built;
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult device_resolved = resolve_device(engine, &device);
        if (device_resolved != STURDY_OK) {
            return device_resolved;
        }
        SFT::WindowManager::WindowGraphicsApi graphics_api{};
        if (!translate_graphics_api(device->backend_type(), &graphics_api)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE,
                             "the active graphics backend has no window-creation counterpart");
        }
        engine_config.graphics_api = graphics_api;

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const SFT::Engine::WindowRequestId id = resolved_engine->window_requests().spawn(engine_config);
        if (out_request_id != nullptr) {
            *out_request_id = id.value;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_recreate_primary(SturdyEngine engine,
                                                             const SturdyWindowConfig *config,
                                                             uint64_t *out_request_id) {
    return guarded([&]() -> SturdyResult {
        // Validated before any handle is consulted, matching every other entry point: a bad
        // argument is a caller bug regardless of engine state.
        SFT::WindowManager::WindowConfig engine_config{};
        const SturdyResult built =
            build_window_config(config, SFT::WindowManager::WindowGraphicsApi::None, &engine_config);
        if (built != STURDY_OK) {
            return built;
        }

        SFT::RHI::RhiDevice *device = nullptr;
        const SturdyResult device_resolved = resolve_device(engine, &device);
        if (device_resolved != STURDY_OK) {
            return device_resolved;
        }
        SFT::WindowManager::WindowGraphicsApi graphics_api{};
        if (!translate_graphics_api(device->backend_type(), &graphics_api)) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE,
                             "the active graphics backend has no window-creation counterpart");
        }
        engine_config.graphics_api = graphics_api;

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        const SFT::Engine::WindowRequestId id =
            resolved_engine->window_requests().recreate_primary_window(engine_config);
        if (out_request_id != nullptr) {
            *out_request_id = id.value;
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_take_completions(SturdyEngine engine,
                                                             SturdyWindowRequestCompletion *out_completions,
                                                             uint32_t capacity,
                                                             uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output count pointer must not be null");
        }
        if (out_completions == nullptr && capacity != 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "output buffer must not be null when capacity is nonzero");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        const SturdyResult resolved = resolve_engine(engine, &resolved_engine);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        const std::vector<SFT::Engine::WindowRequestCompletion> completions =
            resolved_engine->window_requests().take_completions();
        *out_count = static_cast<uint32_t>(completions.size());
        const uint32_t to_copy = std::min<uint32_t>(capacity, *out_count);
        for (uint32_t i = 0; i < to_copy; ++i) {
            const SFT::Engine::WindowRequestCompletion &completion = completions[i];
            SturdyWindowRequestKind kind = STURDY_WINDOW_REQUEST_SPAWN;
            (void)translate_request_kind(completion.kind, &kind);
            out_completions[i] = SturdyWindowRequestCompletion{};
            out_completions[i].request_id = completion.id.value;
            out_completions[i].kind = kind;
            out_completions[i].accepted = completion.accepted ? STURDY_TRUE : STURDY_FALSE;
            out_completions[i].surface = completion_surface(completion);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_request_close(SturdyEngine engine,
                                                         SturdySurface surface,
                                                         uint64_t *out_request_id) {
    return guarded([&]() -> SturdyResult {
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        const SFT::Engine::WindowRequestId id = requests.close(window_id);
                                        if (out_request_id != nullptr) {
                                            *out_request_id = id.value;
                                        }
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_cursor_icon(SturdyEngine engine,
                                                           SturdySurface surface,
                                                           SturdyCursorIcon icon) {
    return guarded([&]() -> SturdyResult {
        SFT::WindowManager::CursorIcon engine_icon{};
        if (!translate_cursor_icon(icon, &engine_icon)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized cursor icon");
        }
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_cursor_icon(window_id, engine_icon);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_mode(SturdyEngine engine,
                                                    SturdySurface surface,
                                                    SturdyWindowMode mode) {
    return guarded([&]() -> SturdyResult {
        SFT::WindowManager::WindowMode engine_mode{};
        if (!translate_window_mode(mode, &engine_mode)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized window mode");
        }
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_fullscreen(window_id, engine_mode);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_decorated(SturdyEngine engine,
                                                         SturdySurface surface,
                                                         SturdyBool decorated) {
    return guarded([&]() -> SturdyResult {
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_decorated(window_id, decorated != STURDY_FALSE);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_transparent(SturdyEngine engine,
                                                           SturdySurface surface,
                                                           SturdyBool transparent) {
    return guarded([&]() -> SturdyResult {
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_transparent(window_id, transparent != STURDY_FALSE);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_relative_mouse_mode(SturdyEngine engine,
                                                                   SturdySurface surface,
                                                                   SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_relative_mouse_mode(window_id, enabled != STURDY_FALSE);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_mouse_locked(SturdyEngine engine,
                                                            SturdySurface surface,
                                                            SturdyBool locked) {
    return guarded([&]() -> SturdyResult {
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_mouse_locked(window_id, locked != STURDY_FALSE);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_cursor_grabbed(SturdyEngine engine,
                                                              SturdySurface surface,
                                                              SturdyBool grabbed) {
    return guarded([&]() -> SturdyResult {
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_cursor_grabbed(window_id, grabbed != STURDY_FALSE);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_effect(SturdyEngine engine,
                                                      SturdySurface surface,
                                                      SturdyWindowEffectKind kind,
                                                      SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        SFT::WindowManager::WindowEffectKind engine_kind{};
        if (!translate_window_effect_kind(kind, &engine_kind)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized window effect kind");
        }
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_blur(window_id, engine_kind, enabled != STURDY_FALSE);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_text_input_active(SturdyEngine engine,
                                                                  SturdySurface surface,
                                                                  SturdyBool active) {
    return guarded([&]() -> SturdyResult {
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_text_input_active(window_id, active != STURDY_FALSE);
                                    });
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_text_input_area(SturdyEngine engine,
                                                                SturdySurface surface,
                                                                const SturdyTextInputArea *area) {
    return guarded([&]() -> SturdyResult {
        if (area == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "area must not be null");
        }
        if (!std::isfinite(area->x) || !std::isfinite(area->y) || !std::isfinite(area->width) ||
            !std::isfinite(area->height) || !std::isfinite(area->cursor_offset_x)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "text input area must be finite");
        }
        SFT::WindowManager::TextInputArea engine_area{};
        engine_area.x = area->x;
        engine_area.y = area->y;
        engine_area.width = area->width;
        engine_area.height = area->height;
        engine_area.cursor_offset_x = area->cursor_offset_x;
        return queue_window_request(engine, surface,
                                    [&](SFT::Engine::WindowRequests &requests,
                                        SFT::WindowManager::WindowId window_id) {
                                        requests.set_text_input_area(window_id, engine_area);
                                    });
    });
}

} // extern "C"
