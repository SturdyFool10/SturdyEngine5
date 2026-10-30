#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <Engine/WindowRequests.hpp>
#include <WindowManager/Desktop.hpp>
#include <WindowManager/WindowManager.hpp>

// The whole `WindowManager::Window` surface as queued requests, written once so the C ABI, CxxApi and Rust bindings
// share it. Every function copies its arguments into the request (the call returns at once), runs `Window::<method>` on
// the window's thread, and leaves the outcome for `WindowRequests::operation_status(window, <operation>)`. Queries put
// their answer in `WindowOperationStatus::output`.
namespace SFT::Engine::WindowControl {

    using WindowManager::WindowId;
    namespace Desktop = WindowManager::Desktop;

    // -- Basics --
    void set_title(WindowRequests &requests, WindowId window, std::string title);
    void set_position(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position);
    void set_size(WindowRequests &requests, WindowId window, WindowManager::WindowExtent size);
    void set_minimum_size(WindowRequests &requests, WindowId window, WindowManager::WindowExtent size);
    void set_maximum_size(WindowRequests &requests, WindowId window, WindowManager::WindowExtent size);
    void set_resizable(WindowRequests &requests, WindowId window, bool resizable);
    void set_opacity(WindowRequests &requests, WindowId window, f32 opacity);
    void maximize(WindowRequests &requests, WindowId window);
    void minimize(WindowRequests &requests, WindowId window);
    void restore(WindowRequests &requests, WindowId window);
    void raise(WindowRequests &requests, WindowId window);
    void focus(WindowRequests &requests, WindowId window);
    void show(WindowRequests &requests, WindowId window);
    void hide(WindowRequests &requests, WindowId window);

    // -- Behaviour --
    void set_always_on_top(WindowRequests &requests, WindowId window, bool enabled);
    void set_aspect_ratio(WindowRequests &requests, WindowId window, f32 min_ratio, f32 max_ratio);
    void set_mouse_passthrough(WindowRequests &requests, WindowId window, bool enabled);
    void set_focusable(WindowRequests &requests, WindowId window, bool enabled);
    void set_auto_minimize_on_focus_loss(WindowRequests &requests, WindowId window, bool enabled);
    /// `parent` empty detaches. Relates the two windows through their native handles, so it works across providers.
    void set_parent(WindowRequests &requests, WindowId window, std::optional<WindowManager::NativeWindowHandle> parent, bool modal);
    void set_keyboard_grab(WindowRequests &requests, WindowId window, bool enabled);
    void set_mouse_confine_rect(WindowRequests &requests, WindowId window, std::optional<WindowManager::WindowRect> rect);
    void set_cursor_position(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position);
    void set_global_cursor_position(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position);
    void set_mouse_capture(WindowRequests &requests, WindowId window, bool enabled);
    void set_fullscreen_target(WindowRequests &requests, WindowId window, WindowManager::FullscreenTarget target);
    void set_window_type(WindowRequests &requests, WindowId window, WindowManager::WindowType type);
    void set_display_gamma(WindowRequests &requests, WindowId window, std::optional<u32> display_id, f32 gamma);

    // -- Cursor --
    void set_custom_cursor(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<std::byte> rgba, i32 hot_x, i32 hot_y);
    struct AnimatedCursorFrame {
        std::vector<std::byte> rgba;
        u32 duration_ms = 100;
    };
    void set_animated_cursor(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<AnimatedCursorFrame> frames,
                             i32 hot_x, i32 hot_y);
    void clear_custom_cursor(WindowRequests &requests, WindowId window);

    // -- Shape, icon, chrome --
    void set_shape(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<std::byte> coverage);
    void set_icon(WindowRequests &requests, WindowId window, u32 width, u32 height, std::vector<std::byte> rgba);
    void request_attention(WindowRequests &requests, WindowId window, WindowManager::AttentionRequest request);
    void set_taskbar_progress(WindowRequests &requests, WindowId window, WindowManager::TaskbarProgress state, f32 value);
    void set_hit_test_regions(WindowRequests &requests, WindowId window, WindowManager::HitTestRegions regions);
    void show_system_menu(WindowRequests &requests, WindowId window, WindowManager::WindowPosition position);
    void start_text_input_with(WindowRequests &requests, WindowId window, WindowManager::TextInputOptions options);

    // -- Clipboard and queries (read the answer from `operation_status(...).output`) --
    void clipboard_text(WindowRequests &requests, WindowId window);                    // output.text
    void set_clipboard_text(WindowRequests &requests, WindowId window, std::string text);
    void clipboard_mime_types(WindowRequests &requests, WindowId window);              // output.list
    void clipboard_data(WindowRequests &requests, WindowId window, std::string mime_type); // output.bytes
    void set_clipboard_data(WindowRequests &requests, WindowId window, std::string mime_type, std::vector<std::byte> bytes);
    void key_name(WindowRequests &requests, WindowId window, WindowManager::KeyboardKey key);   // output.text
    void key_down(WindowRequests &requests, WindowId window, WindowManager::KeyboardKey key);   // output.flag
    void icc_profile(WindowRequests &requests, WindowId window);                       // output.bytes

    // -- Desktop services parented to this window --

    /// Opens a file dialog as a child of the window (`options.parent` is filled in on the window thread). `callback` runs
    /// once when it closes, possibly on another thread.
    void show_file_dialog(WindowRequests &requests, WindowId window, Desktop::FileDialogOptions options, Desktop::FileDialogCallback callback);
    /// Shows a message box over the window and blocks that window's thread until it is dismissed; the pressed button is
    /// `operation_status(...).output.number`.
    void show_message_box(WindowRequests &requests, WindowId window, Desktop::MessageBoxOptions options);

} // namespace SFT::Engine::WindowControl
