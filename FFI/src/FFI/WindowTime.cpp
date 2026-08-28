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

#include <cmath>

#include <Engine/Engine.hpp>

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

} // extern "C"
