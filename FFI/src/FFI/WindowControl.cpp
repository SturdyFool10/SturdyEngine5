/// C ABI implementation of the window-control surface: the whole `WindowManager::Window` abstraction as queued requests
/// (through `Engine/WindowControl`), the capability and display queries, and the process-wide desktop services (file
/// dialogs, message boxes, screen saver, tray).

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <Engine/Engine.hpp>
#include <Engine/WindowControl.hpp>
#include <WindowManager/Desktop.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    namespace E = SFT::Engine;
    namespace W = SFT::WindowManager;
    using SFT::Ffi::copy_string_out;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    [[nodiscard]] W::WindowId window_of(SturdySurface surface) noexcept { return static_cast<W::WindowId>(surface.id); }

    /// Resolves the engine and runs `fn(requests, window)` — the shape of every queued window request.
    template <typename Fn>
    [[nodiscard]] SturdyResult with_requests(SturdyEngine engine, SturdySurface surface, Fn &&fn) noexcept {
        E::Engine *resolved = nullptr;
        const SturdyResult status = resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        fn(resolved->window_requests(), window_of(surface));
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult find_snapshot(SturdyEngine engine, SturdySurface surface, const E::WindowSnapshot **out) noexcept {
        E::Engine *resolved = nullptr;
        const SturdyResult status = resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        const E::WindowSnapshot *found = resolved->window_state().find(window_of(surface));
        if (found == nullptr) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "no window has that surface");
        }
        *out = found;
        return STURDY_OK;
    }

    [[nodiscard]] std::vector<std::byte> copy_bytes(const uint8_t *data, size_t size) {
        const auto *bytes = reinterpret_cast<const std::byte *>(data);
        return std::vector<std::byte>(bytes, bytes + size);
    }

    [[nodiscard]] SturdyResult need_pixels(uint32_t width, uint32_t height, const void *data) noexcept {
        if (width == 0 || height == 0 || data == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "an image needs a non-zero size and pixels");
        }
        return STURDY_OK;
    }

    [[nodiscard]] size_t rgba_size(uint32_t width, uint32_t height) noexcept { return static_cast<size_t>(width) * height * 4; }

    [[nodiscard]] W::WindowRect to_rect(const SturdyWindowRect &rect) noexcept { return W::WindowRect{rect.x, rect.y, rect.width, rect.height}; }

    [[nodiscard]] SturdyWindowRect from_rect(const W::WindowRect &rect) noexcept { return SturdyWindowRect{rect.x, rect.y, rect.width, rect.height}; }

    [[nodiscard]] SturdyDisplayMode from_mode(const W::DisplayMode &mode) noexcept {
        return SturdyDisplayMode{mode.width, mode.height, mode.refresh_rate_hz, mode.pixel_density};
    }

    [[nodiscard]] SturdyWindowErrorKind error_kind(W::WindowErrorCode code) noexcept {
        switch (code) {
            case W::WindowErrorCode::Unsupported: return STURDY_WINDOW_ERROR_UNSUPPORTED;
            case W::WindowErrorCode::InvalidArgument: return STURDY_WINDOW_ERROR_INVALID_ARGUMENT;
            default: return STURDY_WINDOW_ERROR_FAILED;
        }
    }

    [[nodiscard]] std::optional<E::WindowOperation> to_operation(SturdyWindowOperation operation) noexcept {
        if (static_cast<uint32_t>(operation) >= static_cast<uint32_t>(STURDY_WINDOW_OP_COUNT)) {
            return std::nullopt;
        }
        static_assert(static_cast<uint32_t>(STURDY_WINDOW_OP_COUNT) == static_cast<uint32_t>(E::WindowOperation::Count),
                      "the ABI operation list must mirror the engine's");
        return static_cast<E::WindowOperation>(operation);
    }

    /// Reads the last recorded outcome of an operation, or reports that none exists yet.
    [[nodiscard]] SturdyResult read_status(SturdyEngine engine, SturdySurface surface, SturdyWindowOperation operation,
                                           std::optional<E::WindowOperationStatus> *out) noexcept {
        const auto op = to_operation(operation);
        if (!op) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized window operation");
        }
        E::Engine *resolved = nullptr;
        const SturdyResult status = resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        *out = resolved->window_requests().operation_status(window_of(surface), *op);
        return STURDY_OK;
    }

    // -- tray ---------------------------------------------------------------------------------------------------------

    struct TrayRecord {
        std::unique_ptr<W::Desktop::Tray> tray;
        SturdyTrayCallback callback = nullptr;
        void *user = nullptr;
    };
    std::mutex g_tray_mutex;
    std::map<uint64_t, std::shared_ptr<TrayRecord>> g_trays;

    [[nodiscard]] std::vector<W::Desktop::TrayEntry> to_tray_entries(const SturdyTrayEntry *entries, uint32_t count,
                                                                     const std::shared_ptr<TrayRecord> &record) {
        std::vector<W::Desktop::TrayEntry> out;
        for (uint32_t i = 0; entries != nullptr && i < count; ++i) {
            const SturdyTrayEntry &source = entries[i];
            W::Desktop::TrayEntry entry;
            entry.label = source.label != nullptr ? source.label : "";
            entry.separator = source.separator != STURDY_FALSE;
            entry.checkable = source.checkable != STURDY_FALSE;
            entry.checked = source.checked != STURDY_FALSE;
            entry.enabled = source.enabled != STURDY_FALSE;
            if (source.submenu != nullptr && source.submenu_count > 0) {
                entry.submenu = to_tray_entries(source.submenu, source.submenu_count, record);
            } else {
                // The record is held weakly by the click handler so a destroyed tray cannot be called back.
                entry.on_click = [weak = std::weak_ptr<TrayRecord>(record), id = source.id] {
                    if (const auto locked = weak.lock(); locked && locked->callback != nullptr) {
                        locked->callback(locked->user, id);
                    }
                };
            }
            out.push_back(std::move(entry));
        }
        return out;
    }

    [[nodiscard]] SturdyResult resolve_tray(SturdyTray tray, std::shared_ptr<TrayRecord> *out) noexcept {
        const std::lock_guard lock{g_tray_mutex};
        const auto found = g_trays.find(tray.token);
        if (found == g_trays.end()) {
            return set_error(STURDY_ERROR_INVALID_HANDLE, "unknown or destroyed tray");
        }
        *out = found->second;
        return STURDY_OK;
    }

    // -- dialog options -----------------------------------------------------------------------------------------------

    [[nodiscard]] SturdyResult translate_dialog_options(const SturdyFileDialogOptions *options, SturdyFileDialogCallback callback,
                                                        W::Desktop::FileDialogOptions *out) {
        if (options == nullptr || callback == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "options and callback must not be null");
        }
        if (options->struct_size != sizeof(SturdyFileDialogOptions)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyFileDialogOptions size does not match this engine build");
        }
        if (static_cast<uint32_t>(options->kind) > static_cast<uint32_t>(STURDY_FILE_DIALOG_OPEN_FOLDER)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized file dialog kind");
        }
        out->kind = static_cast<W::Desktop::FileDialogKind>(options->kind);
        out->title = options->title != nullptr ? options->title : "";
        out->accept_label = options->accept_label != nullptr ? options->accept_label : "";
        out->cancel_label = options->cancel_label != nullptr ? options->cancel_label : "";
        out->default_location = options->default_location != nullptr ? options->default_location : "";
        out->allow_multiple = options->allow_multiple != STURDY_FALSE;
        for (uint32_t i = 0; options->filters != nullptr && i < options->filter_count; ++i) {
            out->filters.push_back({options->filters[i].name != nullptr ? options->filters[i].name : "",
                                    options->filters[i].pattern != nullptr ? options->filters[i].pattern : "*"});
        }
        return STURDY_OK;
    }

    /// Adapts the C callback; it receives the paths as C strings valid only during the call.
    [[nodiscard]] W::Desktop::FileDialogCallback adapt_dialog_callback(SturdyFileDialogCallback callback, void *user) {
        return [callback, user](W::Desktop::FileDialogResult result) {
            std::vector<const char *> paths;
            paths.reserve(result.paths.size());
            for (const std::string &path : result.paths) {
                paths.push_back(path.c_str());
            }
            callback(user, paths.data(), static_cast<uint32_t>(paths.size()), result.filter_index, result.cancelled ? STURDY_TRUE : STURDY_FALSE,
                     result.failed ? result.error.c_str() : nullptr);
        };
    }

    [[nodiscard]] SturdyResult translate_box_options(const SturdyMessageBoxOptions *options, W::Desktop::MessageBoxOptions *out) {
        if (options == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "options must not be null");
        }
        if (options->struct_size != sizeof(SturdyMessageBoxOptions)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyMessageBoxOptions size does not match this engine build");
        }
        if (static_cast<uint32_t>(options->kind) > static_cast<uint32_t>(STURDY_MESSAGE_BOX_ERROR)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized message box kind");
        }
        out->kind = static_cast<W::Desktop::MessageBoxKind>(options->kind);
        out->title = options->title != nullptr ? options->title : "";
        out->message = options->message != nullptr ? options->message : "";
        for (uint32_t i = 0; options->buttons != nullptr && i < options->button_count; ++i) {
            out->buttons.emplace_back(options->buttons[i] != nullptr ? options->buttons[i] : "");
        }
        out->default_button = options->default_button;
        out->escape_button = options->escape_button;
        return STURDY_OK;
    }

    [[nodiscard]] SturdyResult translate_window_error(const W::WindowError &error) noexcept {
        return set_error(error.code == W::WindowErrorCode::Unsupported ? STURDY_ERROR_NOT_AVAILABLE : STURDY_ERROR_INTERNAL,
                         std::string{error.message}.c_str());
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_text_input_options_init(SturdyTextInputOptions *options) {
    return guarded([&]() -> SturdyResult {
        if (options == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        *options = SturdyTextInputOptions{};
        options->struct_size = static_cast<uint32_t>(sizeof(SturdyTextInputOptions));
        options->capitalization = STURDY_TEXT_CAPITALIZATION_SENTENCES;
        options->autocorrect = STURDY_TRUE;
        return STURDY_OK;
    });
}

// -- Capability and geometry reads --------------------------------------------------------------------------------------

SturdyResult STURDY_ABI_CALL sturdy_window_feature(SturdyEngine engine, SturdySurface surface, SturdyWindowFeature feature,
                                                   SturdyFeatureSupport *out_support) {
    return guarded([&]() -> SturdyResult {
        if (out_support == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        if (static_cast<uint32_t>(feature) >= static_cast<uint32_t>(STURDY_WINDOW_FEATURE_COUNT)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized window feature");
        }
        static_assert(static_cast<uint32_t>(STURDY_WINDOW_FEATURE_COUNT) == static_cast<uint32_t>(W::WindowFeature::Count),
                      "the ABI feature list must mirror the engine's");
        const E::WindowSnapshot *snapshot = nullptr;
        const SturdyResult found = find_snapshot(engine, surface, &snapshot);
        if (found != STURDY_OK) {
            return found;
        }
        const W::FeatureSupport support = snapshot->feature(static_cast<W::WindowFeature>(feature));
        out_support->supported = support.supported ? STURDY_TRUE : STURDY_FALSE;
        // The notes are static literals with a terminator, so the pointer outlives any call.
        out_support->note = support.note.data() != nullptr ? support.note.data() : "";
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_frame_extents(SturdyEngine engine, SturdySurface surface, SturdyFrameExtents *out_extents) {
    return guarded([&]() -> SturdyResult {
        if (out_extents == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const E::WindowSnapshot *snapshot = nullptr;
        const SturdyResult found = find_snapshot(engine, surface, &snapshot);
        if (found != STURDY_OK) {
            return found;
        }
        *out_extents = SturdyFrameExtents{snapshot->frame_extents.left, snapshot->frame_extents.top, snapshot->frame_extents.right,
                                          snapshot->frame_extents.bottom};
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_safe_area(SturdyEngine engine, SturdySurface surface, SturdyWindowRect *out_area) {
    return guarded([&]() -> SturdyResult {
        if (out_area == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const E::WindowSnapshot *snapshot = nullptr;
        const SturdyResult found = find_snapshot(engine, surface, &snapshot);
        if (found != STURDY_OK) {
            return found;
        }
        *out_area = from_rect(snapshot->safe_area);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_display_id(SturdyEngine engine, SturdySurface surface, SturdyBool *out_has_display,
                                                      uint32_t *out_display_id) {
    return guarded([&]() -> SturdyResult {
        if (out_has_display == nullptr || out_display_id == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointers must not be null");
        }
        const E::WindowSnapshot *snapshot = nullptr;
        const SturdyResult found = find_snapshot(engine, surface, &snapshot);
        if (found != STURDY_OK) {
            return found;
        }
        *out_has_display = snapshot->display_id.has_value() ? STURDY_TRUE : STURDY_FALSE;
        *out_display_id = snapshot->display_id.value_or(0);
        return STURDY_OK;
    });
}

// -- Displays -----------------------------------------------------------------------------------------------------------

SturdyResult STURDY_ABI_CALL sturdy_display_count(SturdyEngine engine, uint32_t *out_count) {
    return guarded([&]() -> SturdyResult {
        if (out_count == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        E::Engine *resolved = nullptr;
        const SturdyResult status = resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        *out_count = static_cast<uint32_t>(resolved->window_state().displays().size());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_display_get(SturdyEngine engine, uint32_t index, SturdyDisplayInfo *out_info) {
    return guarded([&]() -> SturdyResult {
        if (out_info == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        E::Engine *resolved = nullptr;
        const SturdyResult status = resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        const auto displays = resolved->window_state().displays();
        if (index >= displays.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "display index is out of range");
        }
        const W::DisplayInfo &display = displays[index];
        *out_info = SturdyDisplayInfo{};
        out_info->struct_size = static_cast<uint32_t>(sizeof(SturdyDisplayInfo));
        out_info->id = display.id;
        out_info->bounds = from_rect(display.bounds);
        out_info->usable_bounds = from_rect(display.usable_bounds);
        out_info->content_scale = display.content_scale;
        out_info->refresh_rate_hz = display.refresh_rate_hz;
        out_info->hdr_enabled = display.hdr_enabled ? STURDY_TRUE : STURDY_FALSE;
        out_info->primary = display.primary ? STURDY_TRUE : STURDY_FALSE;
        out_info->current_mode = from_mode(display.current_mode);
        out_info->mode_count = static_cast<uint32_t>(display.modes.size());
        out_info->physical_width_mm = display.physical_width_mm;
        out_info->physical_height_mm = display.physical_height_mm;
        out_info->orientation = static_cast<SturdyDisplayOrientation>(display.orientation);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_display_name(SturdyEngine engine, uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        E::Engine *resolved = nullptr;
        const SturdyResult status = resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        const auto displays = resolved->window_state().displays();
        if (index >= displays.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "display index is out of range");
        }
        return copy_string_out(displays[index].name, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_display_mode(SturdyEngine engine, uint32_t index, uint32_t mode_index, SturdyDisplayMode *out_mode) {
    return guarded([&]() -> SturdyResult {
        if (out_mode == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        E::Engine *resolved = nullptr;
        const SturdyResult status = resolve_engine(engine, &resolved);
        if (status != STURDY_OK) {
            return status;
        }
        const auto displays = resolved->window_state().displays();
        if (index >= displays.size() || mode_index >= displays[index].modes.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "display or mode index is out of range");
        }
        *out_mode = from_mode(displays[index].modes[mode_index]);
        return STURDY_OK;
    });
}

// -- Operation outcomes -------------------------------------------------------------------------------------------------

SturdyResult STURDY_ABI_CALL sturdy_window_operation_status(SturdyEngine engine, SturdySurface surface, SturdyWindowOperation operation,
                                                            SturdyWindowOperationStatus *out_status) {
    return guarded([&]() -> SturdyResult {
        if (out_status == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        std::optional<E::WindowOperationStatus> status;
        const SturdyResult read = read_status(engine, surface, operation, &status);
        if (read != STURDY_OK) {
            return read;
        }
        *out_status = SturdyWindowOperationStatus{};
        out_status->struct_size = static_cast<uint32_t>(sizeof(SturdyWindowOperationStatus));
        if (status) {
            out_status->completed = STURDY_TRUE;
            out_status->ok = status->ok ? STURDY_TRUE : STURDY_FALSE;
            out_status->error = status->ok ? STURDY_WINDOW_ERROR_NONE : error_kind(status->code);
            out_status->flag = status->output.flag ? STURDY_TRUE : STURDY_FALSE;
            out_status->text_length = static_cast<uint32_t>(status->output.text.size());
            out_status->bytes_size = static_cast<uint32_t>(status->output.bytes.size());
            out_status->list_count = static_cast<uint32_t>(status->output.list.size());
            out_status->number = static_cast<int32_t>(status->output.number);
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_operation_message(SturdyEngine engine, SturdySurface surface, SturdyWindowOperation operation,
                                                             char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        std::optional<E::WindowOperationStatus> status;
        const SturdyResult read = read_status(engine, surface, operation, &status);
        if (read != STURDY_OK) {
            return read;
        }
        return copy_string_out(status ? std::string_view{status->message} : std::string_view{}, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_operation_text(SturdyEngine engine, SturdySurface surface, SturdyWindowOperation operation,
                                                          char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        std::optional<E::WindowOperationStatus> status;
        const SturdyResult read = read_status(engine, surface, operation, &status);
        if (read != STURDY_OK) {
            return read;
        }
        return copy_string_out(status ? std::string_view{status->output.text} : std::string_view{}, buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_operation_bytes(SturdyEngine engine, SturdySurface surface, SturdyWindowOperation operation,
                                                           uint8_t *buffer, size_t capacity, size_t *out_size) {
    return guarded([&]() -> SturdyResult {
        std::optional<E::WindowOperationStatus> status;
        const SturdyResult read = read_status(engine, surface, operation, &status);
        if (read != STURDY_OK) {
            return read;
        }
        const size_t size = status ? status->output.bytes.size() : 0;
        if (out_size != nullptr) {
            *out_size = size;
        }
        if (buffer == nullptr) {
            return STURDY_OK;
        }
        if (size > 0) {
            std::memcpy(buffer, status->output.bytes.data(), std::min(size, capacity));
        }
        return size > capacity ? set_error(STURDY_ERROR_BUFFER_TOO_SMALL, "the supplied byte buffer is too small") : STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_operation_list_item(SturdyEngine engine, SturdySurface surface, SturdyWindowOperation operation,
                                                              uint32_t index, char *buffer, size_t capacity, size_t *out_length) {
    return guarded([&]() -> SturdyResult {
        std::optional<E::WindowOperationStatus> status;
        const SturdyResult read = read_status(engine, surface, operation, &status);
        if (read != STURDY_OK) {
            return read;
        }
        if (!status || index >= status->output.list.size()) {
            return set_error(STURDY_ERROR_OUT_OF_RANGE, "list index is out of range");
        }
        return copy_string_out(status->output.list[index], buffer, capacity, out_length);
    });
}

// -- Setters ------------------------------------------------------------------------------------------------------------

// Each setter validates its arguments, then queues the matching `WindowControl` call. Spelled out one by one: the
// signatures differ too much for a macro to say anything.
#define STURDY_QUEUE(CALL) \
    return with_requests(engine, surface, [&](E::WindowRequests &requests, W::WindowId window) { CALL; })

SturdyResult STURDY_ABI_CALL sturdy_window_set_title(SturdyEngine engine, SturdySurface surface, const char *title) {
    return guarded([&]() -> SturdyResult {
        if (title == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "title must not be null");
        }
        STURDY_QUEUE(E::WindowControl::set_title(requests, window, title));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_position(SturdyEngine engine, SturdySurface surface, int32_t x, int32_t y) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_position(requests, window, W::WindowPosition{x, y})); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_size(SturdyEngine engine, SturdySurface surface, uint32_t width, uint32_t height) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_size(requests, window, W::WindowExtent{width, height})); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_minimum_size(SturdyEngine engine, SturdySurface surface, uint32_t width, uint32_t height) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_minimum_size(requests, window, W::WindowExtent{width, height})); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_maximum_size(SturdyEngine engine, SturdySurface surface, uint32_t width, uint32_t height) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_maximum_size(requests, window, W::WindowExtent{width, height})); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_resizable(SturdyEngine engine, SturdySurface surface, SturdyBool resizable) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_resizable(requests, window, resizable != STURDY_FALSE)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_opacity(SturdyEngine engine, SturdySurface surface, float opacity) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_opacity(requests, window, opacity)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_maximize(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::maximize(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_minimize(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::minimize(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_restore(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::restore(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_raise(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::raise(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_focus(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::focus(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_show(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::show(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_hide(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::hide(requests, window)); });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_always_on_top(SturdyEngine engine, SturdySurface surface, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_always_on_top(requests, window, enabled != STURDY_FALSE)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_aspect_ratio(SturdyEngine engine, SturdySurface surface, float min_ratio, float max_ratio) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_aspect_ratio(requests, window, min_ratio, max_ratio)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_mouse_passthrough(SturdyEngine engine, SturdySurface surface, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_mouse_passthrough(requests, window, enabled != STURDY_FALSE)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_focusable(SturdyEngine engine, SturdySurface surface, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_focusable(requests, window, enabled != STURDY_FALSE)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_auto_minimize_on_focus_loss(SturdyEngine engine, SturdySurface surface, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        STURDY_QUEUE(E::WindowControl::set_auto_minimize_on_focus_loss(requests, window, enabled != STURDY_FALSE));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_parent(SturdyEngine engine, SturdySurface surface, const SturdySurface *parent, SturdyBool modal) {
    return guarded([&]() -> SturdyResult {
        std::optional<W::NativeWindowHandle> parent_handle;
        if (parent != nullptr) {
            // Parenting relates native handles, which the snapshot publishes for every window.
            const E::WindowSnapshot *snapshot = nullptr;
            const SturdyResult found = find_snapshot(engine, *parent, &snapshot);
            if (found != STURDY_OK) {
                return found;
            }
            if (!snapshot->native_handle.has_value()) {
                return set_error(STURDY_ERROR_NOT_AVAILABLE, "the parent window has no native handle yet");
            }
            parent_handle = snapshot->native_handle;
        }
        STURDY_QUEUE(E::WindowControl::set_parent(requests, window, parent_handle, modal != STURDY_FALSE));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_keyboard_grab(SturdyEngine engine, SturdySurface surface, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_keyboard_grab(requests, window, enabled != STURDY_FALSE)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_mouse_confine_rect(SturdyEngine engine, SturdySurface surface, const SturdyWindowRect *rect) {
    return guarded([&]() -> SturdyResult {
        std::optional<W::WindowRect> confine;
        if (rect != nullptr) {
            confine = to_rect(*rect);
        }
        STURDY_QUEUE(E::WindowControl::set_mouse_confine_rect(requests, window, confine));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_cursor_position(SturdyEngine engine, SturdySurface surface, int32_t x, int32_t y) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_cursor_position(requests, window, W::WindowPosition{x, y})); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_global_cursor_position(SturdyEngine engine, SturdySurface surface, int32_t x, int32_t y) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_global_cursor_position(requests, window, W::WindowPosition{x, y})); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_mouse_capture(SturdyEngine engine, SturdySurface surface, SturdyBool enabled) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::set_mouse_capture(requests, window, enabled != STURDY_FALSE)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_fullscreen_target(SturdyEngine engine, SturdySurface surface, const SturdyFullscreenTarget *target) {
    return guarded([&]() -> SturdyResult {
        W::FullscreenTarget engine_target;
        if (target != nullptr) {
            if (target->has_display != STURDY_FALSE) {
                engine_target.display_id = target->display_id;
            }
            if (target->has_mode != STURDY_FALSE) {
                engine_target.mode = W::DisplayMode{target->mode.width, target->mode.height, target->mode.refresh_rate_hz, target->mode.pixel_density};
            }
        }
        STURDY_QUEUE(E::WindowControl::set_fullscreen_target(requests, window, engine_target));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_window_type(SturdyEngine engine, SturdySurface surface, SturdyWindowType type) {
    return guarded([&]() -> SturdyResult {
        if (static_cast<uint32_t>(type) > static_cast<uint32_t>(STURDY_WINDOW_TYPE_SPLASH)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized window type");
        }
        STURDY_QUEUE(E::WindowControl::set_window_type(requests, window, static_cast<W::WindowType>(type)));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_display_gamma(SturdyEngine engine, SturdySurface surface, SturdyBool has_display, uint32_t display_id,
                                                             float gamma) {
    return guarded([&]() -> SturdyResult {
        std::optional<u32> display;
        if (has_display != STURDY_FALSE) {
            display = display_id;
        }
        STURDY_QUEUE(E::WindowControl::set_display_gamma(requests, window, display, gamma));
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_set_custom_cursor(SturdyEngine engine, SturdySurface surface, uint32_t width, uint32_t height,
                                                             const uint8_t *rgba, int32_t hot_x, int32_t hot_y) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_pixels(width, height, rgba); checked != STURDY_OK) {
            return checked;
        }
        STURDY_QUEUE(E::WindowControl::set_custom_cursor(requests, window, width, height, copy_bytes(rgba, rgba_size(width, height)), hot_x, hot_y));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_animated_cursor(SturdyEngine engine, SturdySurface surface, uint32_t width, uint32_t height,
                                                               const SturdyCursorFrame *frames, uint32_t frame_count, int32_t hot_x, int32_t hot_y) {
    return guarded([&]() -> SturdyResult {
        if (frames == nullptr || frame_count == 0 || width == 0 || height == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "an animated cursor needs a size and at least one frame");
        }
        std::vector<E::WindowControl::AnimatedCursorFrame> copies;
        for (uint32_t i = 0; i < frame_count; ++i) {
            if (frames[i].rgba == nullptr) {
                return set_error(STURDY_ERROR_INVALID_ARGUMENT, "every cursor frame needs pixels");
            }
            copies.push_back({copy_bytes(frames[i].rgba, rgba_size(width, height)), frames[i].duration_ms});
        }
        STURDY_QUEUE(E::WindowControl::set_animated_cursor(requests, window, width, height, std::move(copies), hot_x, hot_y));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_clear_custom_cursor(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::clear_custom_cursor(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_shape(SturdyEngine engine, SturdySurface surface, uint32_t width, uint32_t height,
                                                     const uint8_t *coverage) {
    return guarded([&]() -> SturdyResult {
        if (coverage == nullptr) {
            STURDY_QUEUE(E::WindowControl::set_shape(requests, window, 0, 0, {}));
        }
        if (width == 0 || height == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a shape mask needs a non-zero size");
        }
        STURDY_QUEUE(E::WindowControl::set_shape(requests, window, width, height, copy_bytes(coverage, static_cast<size_t>(width) * height)));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_icon(SturdyEngine engine, SturdySurface surface, uint32_t width, uint32_t height, const uint8_t *rgba) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_pixels(width, height, rgba); checked != STURDY_OK) {
            return checked;
        }
        STURDY_QUEUE(E::WindowControl::set_icon(requests, window, width, height, copy_bytes(rgba, rgba_size(width, height))));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_request_attention(SturdyEngine engine, SturdySurface surface, SturdyAttentionRequest request) {
    return guarded([&]() -> SturdyResult {
        if (static_cast<uint32_t>(request) > static_cast<uint32_t>(STURDY_ATTENTION_UNTIL_FOCUSED)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized attention request");
        }
        STURDY_QUEUE(E::WindowControl::request_attention(requests, window, static_cast<W::AttentionRequest>(request)));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_taskbar_progress(SturdyEngine engine, SturdySurface surface, SturdyTaskbarProgress state, float value) {
    return guarded([&]() -> SturdyResult {
        if (static_cast<uint32_t>(state) > static_cast<uint32_t>(STURDY_TASKBAR_PROGRESS_ERROR)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized taskbar progress state");
        }
        STURDY_QUEUE(E::WindowControl::set_taskbar_progress(requests, window, static_cast<W::TaskbarProgress>(state), value));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_hit_test_regions(SturdyEngine engine, SturdySurface surface, const SturdyHitTestRegions *regions) {
    return guarded([&]() -> SturdyResult {
        if (regions == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "regions must not be null");
        }
        if (regions->struct_size != sizeof(SturdyHitTestRegions)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyHitTestRegions size does not match this engine build");
        }
        W::HitTestRegions engine_regions;
        engine_regions.enabled = regions->enabled != STURDY_FALSE;
        engine_regions.resize_border = regions->resize_border;
        for (uint32_t i = 0; regions->drag != nullptr && i < regions->drag_count; ++i) {
            engine_regions.drag.push_back(to_rect(regions->drag[i]));
        }
        for (uint32_t i = 0; regions->no_drag != nullptr && i < regions->no_drag_count; ++i) {
            engine_regions.no_drag.push_back(to_rect(regions->no_drag[i]));
        }
        STURDY_QUEUE(E::WindowControl::set_hit_test_regions(requests, window, std::move(engine_regions)));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_show_system_menu(SturdyEngine engine, SturdySurface surface, int32_t x, int32_t y) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::show_system_menu(requests, window, W::WindowPosition{x, y})); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_start_text_input_ex(SturdyEngine engine, SturdySurface surface, const SturdyTextInputOptions *options) {
    return guarded([&]() -> SturdyResult {
        if (options == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "options must not be null");
        }
        if (options->struct_size != sizeof(SturdyTextInputOptions)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE, "SturdyTextInputOptions size does not match this engine build");
        }
        if (static_cast<uint32_t>(options->type) > static_cast<uint32_t>(STURDY_TEXT_INPUT_NUMBER_PASSWORD_VISIBLE) ||
            static_cast<uint32_t>(options->capitalization) > static_cast<uint32_t>(STURDY_TEXT_CAPITALIZATION_LETTERS)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized text input option");
        }
        const W::TextInputOptions engine_options{static_cast<W::TextInputType>(options->type), static_cast<W::TextCapitalization>(options->capitalization),
                                                 options->autocorrect != STURDY_FALSE, options->multiline != STURDY_FALSE};
        STURDY_QUEUE(E::WindowControl::start_text_input_with(requests, window, engine_options));
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_query_clipboard_text(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::clipboard_text(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_clipboard_text(SturdyEngine engine, SturdySurface surface, const char *text) {
    return guarded([&]() -> SturdyResult {
        if (text == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "text must not be null");
        }
        STURDY_QUEUE(E::WindowControl::set_clipboard_text(requests, window, text));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_query_clipboard_mime_types(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::clipboard_mime_types(requests, window)); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_query_clipboard_data(SturdyEngine engine, SturdySurface surface, const char *mime_type) {
    return guarded([&]() -> SturdyResult {
        if (mime_type == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "mime_type must not be null");
        }
        STURDY_QUEUE(E::WindowControl::clipboard_data(requests, window, mime_type));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_set_clipboard_data(SturdyEngine engine, SturdySurface surface, const char *mime_type, const uint8_t *bytes,
                                                              size_t size) {
    return guarded([&]() -> SturdyResult {
        if (mime_type == nullptr || (bytes == nullptr && size != 0)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "clipboard data needs a MIME type and its bytes");
        }
        STURDY_QUEUE(E::WindowControl::set_clipboard_data(requests, window, mime_type, size == 0 ? std::vector<std::byte>{} : copy_bytes(bytes, size)));
    });
}
SturdyResult STURDY_ABI_CALL sturdy_window_query_key_name(SturdyEngine engine, SturdySurface surface, int32_t key) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::key_name(requests, window, static_cast<W::KeyboardKey>(key))); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_query_key_down(SturdyEngine engine, SturdySurface surface, int32_t key) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::key_down(requests, window, static_cast<W::KeyboardKey>(key))); });
}
SturdyResult STURDY_ABI_CALL sturdy_window_query_icc_profile(SturdyEngine engine, SturdySurface surface) {
    return guarded([&]() -> SturdyResult { STURDY_QUEUE(E::WindowControl::icc_profile(requests, window)); });
}

// (STURDY_QUEUE stays defined: the parented desktop services below use it too.)

// -- Desktop services ---------------------------------------------------------------------------------------------------

SturdyResult STURDY_ABI_CALL sturdy_desktop_show_file_dialog(SturdyEngine engine, const SturdyFileDialogOptions *options,
                                                             SturdyFileDialogCallback callback, void *user) {
    return guarded([&]() -> SturdyResult {
        (void)engine; // process-wide: no window is involved (see `sturdy_window_show_file_dialog` for a parented one)
        W::Desktop::FileDialogOptions engine_options;
        if (const SturdyResult translated = translate_dialog_options(options, callback, &engine_options); translated != STURDY_OK) {
            return translated;
        }
        const auto started = W::Desktop::show_file_dialog(std::move(engine_options), adapt_dialog_callback(callback, user));
        return started ? STURDY_OK : translate_window_error(started.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_show_file_dialog(SturdyEngine engine, SturdySurface surface, const SturdyFileDialogOptions *options,
                                                            SturdyFileDialogCallback callback, void *user) {
    return guarded([&]() -> SturdyResult {
        W::Desktop::FileDialogOptions engine_options;
        if (const SturdyResult translated = translate_dialog_options(options, callback, &engine_options); translated != STURDY_OK) {
            return translated;
        }
        STURDY_QUEUE(E::WindowControl::show_file_dialog(requests, window, std::move(engine_options), adapt_dialog_callback(callback, user)));
    });
}

SturdyResult STURDY_ABI_CALL sturdy_desktop_show_message_box(SturdyEngine engine, const SturdyMessageBoxOptions *options, int32_t *out_button) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (out_button == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        W::Desktop::MessageBoxOptions engine_options;
        if (const SturdyResult translated = translate_box_options(options, &engine_options); translated != STURDY_OK) {
            return translated;
        }
        const auto pressed = W::Desktop::show_message_box(engine_options);
        if (!pressed) {
            return translate_window_error(pressed.error());
        }
        *out_button = *pressed;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_window_show_message_box(SturdyEngine engine, SturdySurface surface, const SturdyMessageBoxOptions *options) {
    return guarded([&]() -> SturdyResult {
        W::Desktop::MessageBoxOptions engine_options;
        if (const SturdyResult translated = translate_box_options(options, &engine_options); translated != STURDY_OK) {
            return translated;
        }
        STURDY_QUEUE(E::WindowControl::show_message_box(requests, window, std::move(engine_options)));
    });
}

SturdyResult STURDY_ABI_CALL sturdy_desktop_set_screensaver_enabled(SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        const auto result = W::Desktop::set_screensaver_enabled(enabled != STURDY_FALSE);
        return result ? STURDY_OK : translate_window_error(result.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_desktop_screensaver_enabled(SturdyBool *out_enabled) {
    return guarded([&]() -> SturdyResult {
        if (out_enabled == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        const auto result = W::Desktop::screensaver_enabled();
        if (!result) {
            return translate_window_error(result.error());
        }
        *out_enabled = *result ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_desktop_pump_events(void) {
    return guarded([&]() -> SturdyResult {
        W::Desktop::pump_events();
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tray_create(SturdyEngine engine, uint32_t width, uint32_t height, const uint8_t *rgba, const char *tooltip,
                                                const SturdyTrayEntry *entries, uint32_t entry_count, SturdyTrayCallback callback, void *user,
                                                SturdyTray *out_tray) {
    return guarded([&]() -> SturdyResult {
        (void)engine;
        if (out_tray == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        if (const SturdyResult checked = need_pixels(width, height, rgba); checked != STURDY_OK) {
            return checked;
        }
        auto record = std::make_shared<TrayRecord>();
        record->callback = callback;
        record->user = user;
        auto created = W::Desktop::Tray::create(width, height, std::span<const std::byte>{reinterpret_cast<const std::byte *>(rgba), rgba_size(width, height)},
                                                tooltip != nullptr ? tooltip : "", to_tray_entries(entries, entry_count, record));
        if (!created) {
            return translate_window_error(created.error());
        }
        record->tray = std::move(*created);
        const uint64_t token = SFT::Ffi::mint_handle(SFT::Ffi::HandleKind::Tray, record.get());
        {
            const std::lock_guard lock{g_tray_mutex};
            g_trays.emplace(token, std::move(record));
        }
        out_tray->token = token;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tray_set_icon(SturdyTray tray, uint32_t width, uint32_t height, const uint8_t *rgba) {
    return guarded([&]() -> SturdyResult {
        if (const SturdyResult checked = need_pixels(width, height, rgba); checked != STURDY_OK) {
            return checked;
        }
        std::shared_ptr<TrayRecord> record;
        if (const SturdyResult found = resolve_tray(tray, &record); found != STURDY_OK) {
            return found;
        }
        const auto result = record->tray->set_icon(width, height, std::span<const std::byte>{reinterpret_cast<const std::byte *>(rgba), rgba_size(width, height)});
        return result ? STURDY_OK : translate_window_error(result.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tray_set_tooltip(SturdyTray tray, const char *tooltip) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<TrayRecord> record;
        if (const SturdyResult found = resolve_tray(tray, &record); found != STURDY_OK) {
            return found;
        }
        record->tray->set_tooltip(tooltip != nullptr ? tooltip : "");
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tray_set_menu(SturdyTray tray, const SturdyTrayEntry *entries, uint32_t entry_count) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<TrayRecord> record;
        if (const SturdyResult found = resolve_tray(tray, &record); found != STURDY_OK) {
            return found;
        }
        const auto result = record->tray->set_menu(to_tray_entries(entries, entry_count, record));
        return result ? STURDY_OK : translate_window_error(result.error());
    });
}

SturdyResult STURDY_ABI_CALL sturdy_tray_destroy(SturdyTray tray) {
    return guarded([&]() -> SturdyResult {
        std::shared_ptr<TrayRecord> record;
        {
            const std::lock_guard lock{g_tray_mutex};
            const auto found = g_trays.find(tray.token);
            if (found == g_trays.end()) {
                return set_error(STURDY_ERROR_INVALID_HANDLE, "unknown or destroyed tray");
            }
            record = std::move(found->second);
            g_trays.erase(found);
        }
        SFT::Ffi::revoke_handle(tray.token);
        return STURDY_OK; // the Tray (and the callbacks that reach it) is destroyed with `record`
    });
}

} // extern "C"
