#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <string>
#include <vector>
#pragma endregion

#include <WindowManager/WindowFeatures.hpp>
#include <WindowManager/WindowGeometry.hpp>

namespace SFT::WindowManager {

    enum class WindowId : usize; // defined in Window.hpp

    /// A rectangle in window coordinates (logical pixels, origin top-left).
    struct WindowRect {
        i32 x = 0;
        i32 y = 0;
        i32 width = 0;
        i32 height = 0;

        [[nodiscard]] constexpr bool contains(i32 px, i32 py) const noexcept {
            return px >= x && py >= y && px < x + width && py < y + height;
        }
    };

    /// How an undecorated window with a custom title bar behaves under the pointer. The OS performs the move and resize
    /// itself (Win32 `WM_NCHITTEST`, Wayland `xdg_toplevel.move/resize`, X11 `_NET_WM_MOVERESIZE`, macOS window drag),
    /// so snapping, Aero shake and compositor resize feedback all work, and the resize-edge cursors come from the OS.
    ///
    /// Update it whenever the title bar layout changes (every frame is fine; it is a small copy).
    struct HitTestRegions {
        /// Off: the window handles every click itself (the default).
        bool enabled = false;
        /// Width in logical pixels of the invisible resize border inside each window edge (corners resize both axes).
        /// 0 disables resizing from the edges. Ignored while maximized or fullscreen.
        u32 resize_border = 6;
        /// Areas that move the window when dragged (the title bar).
        std::vector<WindowRect> drag;
        /// Areas inside `drag` that stay ordinary client area (the title bar's own buttons, tabs, a search box).
        std::vector<WindowRect> no_drag;
    };

    /// Which part of the window a point is. Pure; shared by every platform's hit-test callback.
    enum class HitTestResult : u8 {
        Normal,
        Draggable,
        ResizeTopLeft,
        ResizeTop,
        ResizeTopRight,
        ResizeRight,
        ResizeBottomRight,
        ResizeBottom,
        ResizeBottomLeft,
        ResizeLeft,
    };

    /// Classifies `point` for a window of `size` (logical pixels). Resize borders win over drag regions, `no_drag`
    /// wins over `drag`.
    [[nodiscard]] HitTestResult classify_hit(const HitTestRegions &regions, WindowExtent size, WindowPosition point,
                                             bool allow_resize) noexcept;

    /// Ask the user to look at the window (taskbar flash / dock bounce / urgency hint).
    enum class AttentionRequest : u8 {
        Cancel,
        Briefly,
        UntilFocused,
    };

    /// Taskbar / dock progress indicator state (Windows taskbar button, KDE/GNOME task manager entries via the
    /// Unity launcher API where supported, macOS dock badge where supported).
    enum class TaskbarProgress : u8 {
        None,
        Indeterminate,
        Normal,
        Paused,
        Error,
    };

    /// The OS-wide light/dark preference.
    enum class SystemTheme : u8 {
        Unknown,
        Light,
        Dark,
    };

    /// One monitor.
    struct DisplayInfo {
        u32 id = 0;
        std::string name;
        /// Desktop-coordinate bounds and the usable area (minus taskbars/docks).
        WindowRect bounds{};
        WindowRect usable_bounds{};
        /// UI scale the OS asks for (1.0 = 100%).
        f32 content_scale = 1.0f;
        /// 0 when unknown.
        f32 refresh_rate_hz = 0.0f;
        bool hdr_enabled = false;
        bool primary = false;
        /// The mode the desktop is running in, and every mode a fullscreen window may ask for on this display.
        DisplayMode current_mode{};
        std::vector<DisplayMode> modes;
        /// Physical size of the panel in millimetres; 0 when the provider cannot tell.
        u32 physical_width_mm = 0;
        u32 physical_height_mm = 0;
        DisplayOrientation orientation = DisplayOrientation::Unknown;
    };

    /// A file path or a piece of text dropped onto a window. See `WindowEventKind::DropFile`.
    struct DroppedItem {
        bool is_text = false;
        std::string data;
    };

    /// Queues `item` for `window` (called by window providers when the OS delivers a drop). Any thread.
    void push_dropped_item(WindowId window, DroppedItem item);
    /// Takes every item dropped on `window` since the last call, in arrival order. Any thread.
    [[nodiscard]] std::vector<DroppedItem> take_dropped_items(WindowId window);

} // namespace SFT::WindowManager
