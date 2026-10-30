#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <WindowManager/WindowError.hpp>

namespace SFT::WindowManager {
    class Window;
}

// Desktop services that belong to the process rather than to one window: file dialogs, message boxes and the screen
// saver. GLFW has none of them and SDL3 has all, so they are provided here once, on SDL's implementation, for an
// application whichever window provider its windows use. A window passed as `parent` is honoured when it is an SDL3
// window; a window from another provider simply gets an unparented dialog.
namespace SFT::WindowManager::Desktop {

    struct FileFilter {
        std::string name;
        /// Extensions without the dot, separated by `;` (`"png;jpg;jpeg"`), or `"*"` for everything.
        std::string pattern;
    };

    enum class FileDialogKind : u8 {
        OpenFile,
        SaveFile,
        OpenFolder,
    };

    struct FileDialogOptions {
        FileDialogKind kind = FileDialogKind::OpenFile;
        std::string title;
        /// Labels of the confirm / cancel buttons; empty uses the system's.
        std::string accept_label;
        std::string cancel_label;
        /// A folder, or a file path to preselect.
        std::string default_location;
        std::vector<FileFilter> filters;
        bool allow_multiple = false;
        Window *parent = nullptr;
    };

    struct FileDialogResult {
        /// The chosen paths; empty when `cancelled` or `failed`.
        std::vector<std::string> paths;
        /// Index into `FileDialogOptions::filters` of the filter that was active, or -1 when the system cannot say.
        int filter_index = -1;
        bool cancelled = false;
        bool failed = false;
        std::string error;
    };

    /// Called exactly once when the dialog closes. It may run on a thread other than the caller's.
    using FileDialogCallback = std::function<void(FileDialogResult)>;

    /// Opens the dialog and returns at once; `callback` reports the outcome. Fails (and never calls `callback`) when the
    /// dialog cannot be started.
    [[nodiscard]] std::expected<void, WindowError> show_file_dialog(FileDialogOptions options, FileDialogCallback callback) noexcept;

    enum class MessageBoxKind : u8 {
        Information,
        Warning,
        Error,
    };

    struct MessageBoxOptions {
        MessageBoxKind kind = MessageBoxKind::Information;
        std::string title;
        std::string message;
        /// Button labels, left to right; empty gives a single "OK".
        std::vector<std::string> buttons;
        /// Button chosen by Enter / Escape (an index into `buttons`), or -1 for none.
        int default_button = 0;
        int escape_button = -1;
        Window *parent = nullptr;
    };

    /// Shows the box and blocks until it is dismissed. Returns the index of the pressed button (`escape_button` or -1
    /// when closed another way). Call it from the main thread.
    [[nodiscard]] std::expected<int, WindowError> show_message_box(const MessageBoxOptions &options) noexcept;

    /// One row of a tray menu.
    struct TrayEntry {
        std::string label;
        /// A separator line; every other field is ignored.
        bool separator = false;
        /// Shows a check mark that toggles when clicked (`on_click` then reads `checked` through the tray).
        bool checkable = false;
        bool checked = false;
        bool enabled = true;
        std::function<void()> on_click;
        /// Non-empty makes this row open a submenu (and `on_click` is ignored).
        std::vector<TrayEntry> submenu;
    };

    /// A notification-area / menu-bar icon with a menu. The window system must have a tray (most Linux desktops need a
    /// StatusNotifier host); creation fails with `Unsupported` where there is none. Create and destroy it on the main
    /// thread. Clicks are delivered while events are pumped: by the SDL provider's pump, or by `pump_events` for programs
    /// whose windows come from another provider (the GLFW provider calls it for you).
    class Tray {
      public:
        ~Tray();
        Tray(const Tray &) = delete;
        Tray &operator=(const Tray &) = delete;

        /// `icon_rgba` is tightly packed RGBA8, `width * height * 4` bytes.
        [[nodiscard]] static std::expected<std::unique_ptr<Tray>, WindowError> create(
            u32 width, u32 height, std::span<const std::byte> icon_rgba, std::string tooltip, std::vector<TrayEntry> entries) noexcept;

        [[nodiscard]] std::expected<void, WindowError> set_icon(u32 width, u32 height, std::span<const std::byte> icon_rgba) noexcept;
        void set_tooltip(const std::string &tooltip) noexcept;
        /// Replaces the whole menu.
        [[nodiscard]] std::expected<void, WindowError> set_menu(std::vector<TrayEntry> entries) noexcept;

      private:
        struct Impl;
        explicit Tray(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /// Delivers tray clicks (and any other SDL-owned desktop events) for a program that is not pumping SDL itself. Cheap
    /// when no tray exists. Call it from the main thread once per frame.
    void pump_events() noexcept;

    /// Whether the system may blank the screen / start the screen saver while the program runs (it may by default).
    /// Games turn this off for their lifetime. Unsupported where the window system cannot be reached (macOS with only
    /// GLFW windows).
    [[nodiscard]] std::expected<void, WindowError> set_screensaver_enabled(bool enabled) noexcept;
    [[nodiscard]] std::expected<bool, WindowError> screensaver_enabled() noexcept;

} // namespace SFT::WindowManager::Desktop
