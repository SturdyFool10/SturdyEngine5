#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <span>
#include <string_view>
#pragma endregion

namespace SFT::WindowManager {

    // The window abstraction is the union of what its providers (SDL3, GLFW) and their platforms can do, in the way an
    // RHI is the union of its backends: every capability any provider has is a `Window` method, a provider implements
    // all of it that its host library offers, backs the rest with the platform's native handle where that is possible,
    // and reports `WindowErrorCode::Unsupported` for what is impossible. `Window::feature_support` says up front which.

    /// A capability a window may or may not have on this provider, OS and session type (X11 vs Wayland).
    enum class WindowFeature : u8 {
        AlwaysOnTop,
        AspectRatio,
        /// Clicks fall through the window to whatever is behind it.
        MousePassthrough,
        Focusable,
        AutoMinimizeOnFocusLoss,
        ParentWindow,
        ModalWindow,
        KeyboardGrab,
        MouseConfineRect,
        WarpCursor,
        CustomCursor,
        FrameExtents,
        SafeArea,
        /// Choosing the display and display mode a fullscreen window uses.
        FullscreenTarget,
        TextInputOptions,
        ScreenKeyboard,
        ClipboardMimeTypes,
        TypedClipboard,
        HitTestRegions,
        SystemMenu,
        WindowIcon,
        RequestAttention,
        TaskbarProgress,
        SystemTheme,
        DisplayList,
        DisplayModes,
        DisplayEvents,
        LiveResizeCallback,
        WaitForEvents,
        FileDrop,
        TextDrop,
        /// `Window::set_window_type`: how the window manager treats utility palettes, tooltips and popup menus.
        WindowTypeHint,
        /// `Window::set_shape`: a non-rectangular window outline from an alpha mask.
        WindowShape,
        AnimatedCursor,
        /// `Window::key_name` / `key_down`.
        KeyboardQuery,
        /// `Window::set_global_cursor_position`.
        WarpCursorGlobal,
        /// The mouse keeps reporting while a button is held outside the window.
        MouseCapture,
        /// `Window::icc_profile`.
        IccProfile,
        /// `Desktop::Tray`.
        SystemTray,
        /// Touch screens and pads deliver `Touch*` events.
        TouchInput,
        /// Pens and tablets deliver `Pen*` events.
        PenInput,
        Count,
    };

    /// How the window manager should treat a window (the EWMH window types, which Windows and macOS approximate with
    /// extended styles and window levels).
    enum class WindowType : u8 {
        Normal,
        /// A tool palette: no taskbar entry, lighter frame.
        Utility,
        Tooltip,
        PopupMenu,
        DropdownMenu,
        Dialog,
        Splash,
    };

    enum class DisplayOrientation : u8 {
        Unknown,
        Landscape,
        LandscapeFlipped,
        Portrait,
        PortraitFlipped,
    };

    /// A window outline: `width * height` coverage values (0 = outside, anything above 127 = inside), scaled to the
    /// window size when applied. Empty `coverage` restores the rectangle.
    struct WindowShapeMask {
        u32 width = 0;
        u32 height = 0;
        std::span<const std::byte> coverage;
    };


    [[nodiscard]] constexpr std::string_view window_feature_name(WindowFeature feature) noexcept {
        switch (feature) {
            case WindowFeature::AlwaysOnTop: return "AlwaysOnTop";
            case WindowFeature::AspectRatio: return "AspectRatio";
            case WindowFeature::MousePassthrough: return "MousePassthrough";
            case WindowFeature::Focusable: return "Focusable";
            case WindowFeature::AutoMinimizeOnFocusLoss: return "AutoMinimizeOnFocusLoss";
            case WindowFeature::ParentWindow: return "ParentWindow";
            case WindowFeature::ModalWindow: return "ModalWindow";
            case WindowFeature::KeyboardGrab: return "KeyboardGrab";
            case WindowFeature::MouseConfineRect: return "MouseConfineRect";
            case WindowFeature::WarpCursor: return "WarpCursor";
            case WindowFeature::CustomCursor: return "CustomCursor";
            case WindowFeature::FrameExtents: return "FrameExtents";
            case WindowFeature::SafeArea: return "SafeArea";
            case WindowFeature::FullscreenTarget: return "FullscreenTarget";
            case WindowFeature::TextInputOptions: return "TextInputOptions";
            case WindowFeature::ScreenKeyboard: return "ScreenKeyboard";
            case WindowFeature::ClipboardMimeTypes: return "ClipboardMimeTypes";
            case WindowFeature::TypedClipboard: return "TypedClipboard";
            case WindowFeature::HitTestRegions: return "HitTestRegions";
            case WindowFeature::SystemMenu: return "SystemMenu";
            case WindowFeature::WindowIcon: return "WindowIcon";
            case WindowFeature::RequestAttention: return "RequestAttention";
            case WindowFeature::TaskbarProgress: return "TaskbarProgress";
            case WindowFeature::SystemTheme: return "SystemTheme";
            case WindowFeature::DisplayList: return "DisplayList";
            case WindowFeature::DisplayModes: return "DisplayModes";
            case WindowFeature::DisplayEvents: return "DisplayEvents";
            case WindowFeature::LiveResizeCallback: return "LiveResizeCallback";
            case WindowFeature::WaitForEvents: return "WaitForEvents";
            case WindowFeature::FileDrop: return "FileDrop";
            case WindowFeature::TextDrop: return "TextDrop";
            case WindowFeature::WindowTypeHint: return "WindowTypeHint";
            case WindowFeature::WindowShape: return "WindowShape";
            case WindowFeature::AnimatedCursor: return "AnimatedCursor";
            case WindowFeature::KeyboardQuery: return "KeyboardQuery";
            case WindowFeature::WarpCursorGlobal: return "WarpCursorGlobal";
            case WindowFeature::MouseCapture: return "MouseCapture";
            case WindowFeature::IccProfile: return "IccProfile";
            case WindowFeature::SystemTray: return "SystemTray";
            case WindowFeature::TouchInput: return "TouchInput";
            case WindowFeature::PenInput: return "PenInput";
            case WindowFeature::Count: break;
        }
        return "Unknown";
    }

    /// Whether a feature is available, with a static-literal reason when it is not (or only partly).
    struct FeatureSupport {
        bool supported = false;
        /// Why not, or what is reduced when it is partial. Static literal; empty when fully supported.
        std::string_view note;

        [[nodiscard]] static constexpr FeatureSupport yes(std::string_view note = {}) noexcept { return {true, note}; }
        [[nodiscard]] static constexpr FeatureSupport no(std::string_view note) noexcept { return {false, note}; }
    };

    /// The thickness of the window manager's decorations around the client area, in window units.
    struct FrameExtents {
        i32 left = 0;
        i32 top = 0;
        i32 right = 0;
        i32 bottom = 0;
    };

    /// A cursor image: tightly packed RGBA8 (`width * height * 4` bytes) and the hot spot in image pixels.
    struct CursorImage {
        u32 width = 0;
        u32 height = 0;
        std::span<const std::byte> rgba;
        i32 hot_x = 0;
        i32 hot_y = 0;
    };

    /// One frame of an animated cursor.
    struct CursorFrame {
        CursorImage image;
        u32 duration_ms = 100;
    };

    enum class TextInputType : u8 {
        Text,
        Name,
        Email,
        Username,
        PasswordHidden,
        PasswordVisible,
        Number,
        NumberPasswordHidden,
        NumberPasswordVisible,
    };

    enum class TextCapitalization : u8 {
        None,
        Sentences,
        Words,
        Letters,
    };

    /// What kind of text a field takes, for on-screen keyboards and IMEs that adapt to it.
    struct TextInputOptions {
        TextInputType type = TextInputType::Text;
        TextCapitalization capitalization = TextCapitalization::Sentences;
        bool autocorrect = true;
        bool multiline = false;
    };

    /// One video mode of a display.
    struct DisplayMode {
        u32 width = 0;
        u32 height = 0;
        f32 refresh_rate_hz = 0.0f;
        f32 pixel_density = 1.0f;

        [[nodiscard]] constexpr bool operator==(const DisplayMode &) const noexcept = default;
    };

    /// Where, and in which mode, a fullscreen window goes. An unset field means "the window's current display" /
    /// "the display's desktop mode".
    struct FullscreenTarget {
        std::optional<u32> display_id;
        std::optional<DisplayMode> mode;
    };

} // namespace SFT::WindowManager
