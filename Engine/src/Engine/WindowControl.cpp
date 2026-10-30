#include <Engine/WindowControl.hpp>

#include <utility>

namespace SFT::Engine::WindowControl {

    namespace {

        using WindowManager::Window;
        using Result = std::expected<void, WindowManager::WindowError>;

        /// Queues a closure that needs no output.
        template <typename Fn>
        void queue(WindowRequests &requests, WindowId window, WindowOperation operation, Fn &&fn) {
            requests.run(window, operation,
                         [fn = std::forward<Fn>(fn)](Window &w, WindowOperationOutput &) mutable -> Result { return fn(w); });
        }

    } // namespace

    void set_title(WindowRequests &requests, WindowId window, std::string title) {
        queue(requests, window, WindowOperation::Title, [title = std::move(title)](Window &w) { return w.set_title(title.c_str()); });
    }
    void set_position(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position) {
        queue(requests, window, WindowOperation::Position, [position](Window &w) { return w.set_position(position); });
    }
    void set_size(WindowRequests &requests, WindowId window, WindowManager::WindowExtent size) {
        queue(requests, window, WindowOperation::Size, [size](Window &w) { return w.set_size(size); });
    }
    void set_minimum_size(WindowRequests &requests, WindowId window, WindowManager::WindowExtent size) {
        queue(requests, window, WindowOperation::MinimumSize, [size](Window &w) { return w.set_minimum_size(size); });
    }
    void set_maximum_size(WindowRequests &requests, WindowId window, WindowManager::WindowExtent size) {
        queue(requests, window, WindowOperation::MaximumSize, [size](Window &w) { return w.set_maximum_size(size); });
    }
    void set_resizable(WindowRequests &requests, WindowId window, bool resizable) {
        queue(requests, window, WindowOperation::Resizable, [resizable](Window &w) { return w.set_resizable(resizable); });
    }
    void set_opacity(WindowRequests &requests, WindowId window, f32 opacity) {
        queue(requests, window, WindowOperation::Opacity, [opacity](Window &w) { return w.set_opacity(opacity); });
    }
    void maximize(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::Maximize, [](Window &w) { return w.maximize(); });
    }
    void minimize(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::Minimize, [](Window &w) { return w.minimize(); });
    }
    void restore(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::Restore, [](Window &w) { return w.restore(); });
    }
    void raise(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::Raise, [](Window &w) { return w.raise(); });
    }
    void focus(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::Focus, [](Window &w) { return w.focus(); });
    }
    void show(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::Show, [](Window &w) { return w.show(); });
    }
    void hide(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::Hide, [](Window &w) { return w.hide(); });
    }

    void set_always_on_top(WindowRequests &requests, WindowId window, bool enabled) {
        queue(requests, window, WindowOperation::AlwaysOnTop, [enabled](Window &w) { return w.set_always_on_top(enabled); });
    }
    void set_aspect_ratio(WindowRequests &requests, WindowId window, f32 min_ratio, f32 max_ratio) {
        queue(requests, window, WindowOperation::AspectRatio, [=](Window &w) { return w.set_aspect_ratio(min_ratio, max_ratio); });
    }
    void set_mouse_passthrough(WindowRequests &requests, WindowId window, bool enabled) {
        queue(requests, window, WindowOperation::MousePassthrough, [enabled](Window &w) { return w.set_mouse_passthrough(enabled); });
    }
    void set_focusable(WindowRequests &requests, WindowId window, bool enabled) {
        queue(requests, window, WindowOperation::Focusable, [enabled](Window &w) { return w.set_focusable(enabled); });
    }
    void set_auto_minimize_on_focus_loss(WindowRequests &requests, WindowId window, bool enabled) {
        queue(requests, window, WindowOperation::AutoMinimize, [enabled](Window &w) { return w.set_auto_minimize_on_focus_loss(enabled); });
    }
    void set_parent(WindowRequests &requests, WindowId window, std::optional<WindowManager::NativeWindowHandle> parent, bool modal) {
        queue(requests, window, WindowOperation::Parent, [parent, modal](Window &w) -> Result {
            const auto child = w.native_window_handle();
            if (!child) {
                return std::unexpected(child.error());
            }
            const WindowManager::WindowEffectResult applied = WindowManager::set_native_window_parent(*child, parent, modal);
            return WindowManager::window_result_from_effect_result(applied);
        });
    }
    void set_keyboard_grab(WindowRequests &requests, WindowId window, bool enabled) {
        queue(requests, window, WindowOperation::KeyboardGrab, [enabled](Window &w) { return w.set_keyboard_grab(enabled); });
    }
    void set_mouse_confine_rect(WindowRequests &requests, WindowId window, std::optional<WindowManager::WindowRect> rect) {
        queue(requests, window, WindowOperation::MouseConfineRect, [rect](Window &w) { return w.set_mouse_confine_rect(rect); });
    }
    void set_cursor_position(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position) {
        queue(requests, window, WindowOperation::CursorPosition, [position](Window &w) { return w.set_cursor_position(position); });
    }
    void set_global_cursor_position(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position) {
        queue(requests, window, WindowOperation::GlobalCursorPosition, [position](Window &w) { return w.set_global_cursor_position(position); });
    }
    void set_mouse_capture(WindowRequests &requests, WindowId window, bool enabled) {
        queue(requests, window, WindowOperation::MouseCapture, [enabled](Window &w) { return w.set_mouse_capture(enabled); });
    }
    void set_fullscreen_target(WindowRequests &requests, WindowId window, WindowManager::FullscreenTarget target) {
        queue(requests, window, WindowOperation::FullscreenTarget, [target](Window &w) { return w.set_fullscreen_target(target); });
    }
    void set_window_type(WindowRequests &requests, WindowId window, WindowManager::WindowType type) {
        queue(requests, window, WindowOperation::WindowType, [type](Window &w) { return w.set_window_type(type); });
    }
    void set_display_gamma(WindowRequests &requests, WindowId window, std::optional<u32> display_id, f32 gamma) {
        queue(requests, window, WindowOperation::DisplayGamma, [display_id, gamma](Window &w) { return w.set_display_gamma(display_id, gamma); });
    }

    void set_custom_cursor(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<std::byte> rgba, i32 hot_x, i32 hot_y) {
        queue(requests, window, WindowOperation::CustomCursor, [=, rgba = std::move(rgba)](Window &w) {
            return w.set_custom_cursor(WindowManager::CursorImage{width, height, rgba, hot_x, hot_y});
        });
    }
    void set_animated_cursor(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<AnimatedCursorFrame> frames,
                             i32 hot_x, i32 hot_y) {
        queue(requests, window, WindowOperation::AnimatedCursor, [=, frames = std::move(frames)](Window &w) {
            std::vector<WindowManager::CursorFrame> views;
            views.reserve(frames.size());
            for (const AnimatedCursorFrame &frame : frames) {
                views.push_back(WindowManager::CursorFrame{WindowManager::CursorImage{width, height, frame.rgba, hot_x, hot_y}, frame.duration_ms});
            }
            return w.set_animated_cursor(views);
        });
    }
    void clear_custom_cursor(WindowRequests &requests, WindowId window) {
        queue(requests, window, WindowOperation::ClearCustomCursor, [](Window &w) { return w.clear_custom_cursor(); });
    }

    void set_shape(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<std::byte> coverage) {
        queue(requests, window, WindowOperation::Shape, [=, coverage = std::move(coverage)](Window &w) {
            return w.set_shape(WindowManager::WindowShapeMask{width, height, coverage});
        });
    }
    void set_icon(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<std::byte> rgba) {
        queue(requests, window, WindowOperation::Icon, [=, rgba = std::move(rgba)](Window &w) { return w.set_icon(width, height, rgba); });
    }
    void request_attention(WindowRequests &requests, WindowId window, WindowManager::AttentionRequest request) {
        queue(requests, window, WindowOperation::Attention, [request](Window &w) { return w.request_attention(request); });
    }
    void set_taskbar_progress(WindowRequests &requests, WindowId window, WindowManager::TaskbarProgress state, f32 value) {
        queue(requests, window, WindowOperation::TaskbarProgress, [state, value](Window &w) { return w.set_taskbar_progress(state, value); });
    }
    void set_hit_test_regions(WindowRequests &requests, WindowId window, WindowManager::HitTestRegions regions) {
        queue(requests, window, WindowOperation::HitTestRegions, [regions = std::move(regions)](Window &w) { return w.set_hit_test_regions(regions); });
    }
    void show_system_menu(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position) {
        queue(requests, window, WindowOperation::SystemMenu, [position](Window &w) { return w.show_system_menu(position); });
    }
    void start_text_input_with(WindowRequests &requests, WindowId window, WindowManager::TextInputOptions options) {
        queue(requests, window, WindowOperation::TextInputOptions, [options](Window &w) { return w.start_text_input_with(options); });
    }

    void clipboard_text(WindowRequests &requests, WindowId window) {
        requests.run(window, WindowOperation::ClipboardText, [](Window &w, WindowOperationOutput &out) -> Result {
            out.text = w.clipboard_text();
            return {};
        });
    }
    void set_clipboard_text(WindowRequests &requests, WindowId window, std::string text) {
        queue(requests, window, WindowOperation::SetClipboardText, [text = std::move(text)](Window &w) { return w.set_clipboard_text(text); });
    }
    void clipboard_mime_types(WindowRequests &requests, WindowId window) {
        requests.run(window, WindowOperation::ClipboardMimeTypes, [](Window &w, WindowOperationOutput &out) -> Result {
            out.list = w.clipboard_mime_types();
            return {};
        });
    }
    void clipboard_data(WindowRequests &requests, WindowId window, std::string mime_type) {
        requests.run(window, WindowOperation::ClipboardData, [mime_type = std::move(mime_type)](Window &w, WindowOperationOutput &out) -> Result {
            out.flag = w.clipboard_has_mime(mime_type);
            out.bytes = w.clipboard_data(mime_type);
            return {};
        });
    }
    void set_clipboard_data(WindowRequests &requests, WindowId window, std::string mime_type, std::vector<std::byte> bytes) {
        queue(requests, window, WindowOperation::SetClipboardData,
              [mime_type = std::move(mime_type), bytes = std::move(bytes)](Window &w) { return w.set_clipboard_data(mime_type, bytes); });
    }
    void key_name(WindowRequests &requests, WindowId window, WindowManager::KeyboardKey key) {
        requests.run(window, WindowOperation::KeyName, [key](Window &w, WindowOperationOutput &out) -> Result {
            out.text = w.key_name(key);
            return {};
        });
    }
    void key_down(WindowRequests &requests, WindowId window, WindowManager::KeyboardKey key) {
        requests.run(window, WindowOperation::KeyDown, [key](Window &w, WindowOperationOutput &out) -> Result {
            out.flag = w.key_down(key);
            return {};
        });
    }
    void show_file_dialog(WindowRequests &requests, WindowId window, Desktop::FileDialogOptions options, Desktop::FileDialogCallback callback) {
        requests.run(window, WindowOperation::FileDialog,
                     [options = std::move(options), callback = std::move(callback)](Window &w, WindowOperationOutput &) mutable -> Result {
                         options.parent = &w; // the window object only exists on the window thread
                         return Desktop::show_file_dialog(std::move(options), std::move(callback));
                     });
    }

    void show_message_box(WindowRequests &requests, WindowId window, Desktop::MessageBoxOptions options) {
        requests.run(window, WindowOperation::MessageBox,
                     [options = std::move(options)](Window &w, WindowOperationOutput &out) mutable -> Result {
                         options.parent = &w;
                         const auto pressed = Desktop::show_message_box(options);
                         if (!pressed) {
                             return std::unexpected(pressed.error());
                         }
                         out.number = *pressed;
                         return {};
                     });
    }

    void icc_profile(WindowRequests &requests, WindowId window) {
        requests.run(window, WindowOperation::IccProfile, [](Window &w, WindowOperationOutput &out) -> Result {
            auto profile = w.icc_profile();
            if (!profile) {
                return std::unexpected(profile.error());
            }
            out.bytes = std::move(*profile);
            return {};
        });
    }

} // namespace SFT::Engine::WindowControl
