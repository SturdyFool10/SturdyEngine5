#pragma once

// Plain-data window types. bindgen-safe (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

#include <CxxApi/Types/Runtime.hpp>

namespace SFT::CxxApi {

    /// A signed 2D integer that may be absent (`ok == false`).
    struct Int2Result {
        bool ok = false;
        std::int32_t x = 0;
        std::int32_t y = 0;
    };

    /// `WindowManager::WindowHdrProperties`, or `available == false` when the window can't report them.
    struct HdrProperties {
        bool available = false;
        bool hdr_enabled = false;
        float sdr_white_level = 1.0f;
        float hdr_headroom = 1.0f;
    };

    /// `Engine::WindowSnapshot`: the observed state of one managed window.
    struct WindowSnapshot {
        std::uint64_t id = 0;
        /// Window coordinates (mouse positions, `position_*` use this space).
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        /// Physical pixels.
        std::uint32_t framebuffer_width = 0;
        std::uint32_t framebuffer_height = 0;
        std::int32_t position_x = 0;
        std::int32_t position_y = 0;
        /// OS UI scale of the window's display (1.0 = 100%).
        float content_scale = 1.0f;
        float opacity = 1.0f;
        bool mouse_locked = false;
        bool focused = false;
        bool primary = false;
    };

    /// Mirrors `WindowManager::CursorIcon`.
    enum class CursorIcon : std::uint32_t {
        Default = 0,
        Pointer = 1,
        Text = 2,
        Grab = 3,
        Grabbing = 4,
        ResizeHorizontal = 5,
        ResizeVertical = 6,
        ResizeNwse = 7,
        ResizeNesw = 8,
        NotAllowed = 9,
    };

    /// Mirrors `WindowManager::WindowEffectKind`.
    enum class WindowEffect : std::uint32_t {
        Blur = 0,
        Acrylic = 1,
        Mica = 2,
        MicaAlt = 3,
        Tabbed = 4,
        DarkMode = 5,
        BorderColor = 6,
        CaptionColor = 7,
        TextColor = 8,
        Transparent = 9,
        BackdropAuto = 10,
        BackdropNone = 11,
        AccentBlur = 12,
        AccentAcrylic = 13,
        CornerPreference = 14,
        VisualEffect = 15,
        Opacity = 16,
        TransitionsDisabled = 17,
        TitlebarTransparent = 18,
        WindowShadow = 19,
        GlassEffect = 20,
        CornerRadius = 21,
    };

    /// Mirrors `WindowManager::LinuxBlurProtocol`.
    enum class LinuxBlurProtocol : std::uint32_t { Automatic = 0, ExtBackgroundEffect = 1, KdeBlur = 2, KdeX11 = 3 };

    /// Mirrors `WindowManager::WindowCornerPreference` (Windows 11).
    enum class WindowCornerPreference : std::uint32_t { Default = 0, DoNotRound = 1, Round = 2, RoundSmall = 3 };

    /// Mirrors `WindowManager::MacVisualEffectMaterial` (`NSVisualEffectMaterial`).
    enum class MacMaterial : std::uint32_t {
        Titlebar = 0, Selection, Menu, Popover, Sidebar, HeaderView, Sheet, WindowBackground, HudWindow, FullScreenUi,
        ToolTip, ContentBackground, UnderWindowBackground, UnderPageBackground,
    };

    /// Mirrors `WindowManager::MacBlendingMode`.
    enum class MacBlendingMode : std::uint32_t { BehindWindow = 0, WithinWindow = 1 };

    /// Mirrors `WindowManager::MacVisualEffectState`.
    enum class MacEffectState : std::uint32_t { FollowsWindowActiveState = 0, Active = 1, Inactive = 2 };

    /// Mirrors `WindowManager::MacAppearance`.
    enum class MacAppearance : std::uint32_t { Inherit = 0, Aqua, DarkAqua, VibrantLight, VibrantDark };

    /// Mirrors `WindowManager::MacGlassStyle` (`NSGlassEffectViewStyle`, macOS 26).
    enum class MacGlassStyle : std::uint32_t { Regular = 0, Clear = 1 };

    /// Everything beyond kind and on/off for a window effect request; each field applies to specific effects and is
    /// ignored by the rest. Blur regions are not part of this: a blur covers the whole window.
    struct WindowEffectOptions {
        /// Linux blur: which compositor protocol to use.
        LinuxBlurProtocol linux_blur_protocol = LinuxBlurProtocol::Automatic;
        /// `BorderColor`, `CaptionColor`, `TextColor`: ARGB.
        std::uint32_t color_argb = 0;
        /// `Blur` (Linux/Windows): round the blurred region's corners by this many logical pixels.
        float blur_corner_radius = 0.0f;
        /// `AccentBlur`, `AccentAcrylic`, `GlassEffect`: tint, ARGB (alpha = tint opacity; glass with alpha 0 is untinted).
        std::uint32_t tint_argb = 0;
        /// Accent effects: draw the window border on these sides.
        bool accent_border_left = true;
        bool accent_border_top = true;
        bool accent_border_right = true;
        bool accent_border_bottom = true;
        /// `BorderColor` (Windows 11): draw no border instead of using `color_argb`.
        bool hide_border = false;
        /// `CornerPreference` (Windows 11).
        WindowCornerPreference corner_preference = WindowCornerPreference::Default;
        /// `VisualEffect` (macOS).
        MacMaterial mac_material = MacMaterial::UnderWindowBackground;
        MacBlendingMode mac_blending = MacBlendingMode::BehindWindow;
        MacEffectState mac_state = MacEffectState::FollowsWindowActiveState;
        bool mac_emphasized = false;
        MacAppearance mac_appearance = MacAppearance::Inherit;
        /// `GlassEffect` (macOS 26): variant and corner radius in logical points.
        MacGlassStyle mac_glass_style = MacGlassStyle::Regular;
        float mac_glass_corner_radius = 0.0f;
        /// `Opacity`: 0 (invisible) to 1 (opaque).
        float opacity = 1.0f;
        /// `CornerRadius`: the window outline's corner radius in logical pixels (0 = square).
        float window_corner_radius = 0.0f;
    };

    /// Mirrors `WindowManager::WindowFeature`: what a window can do on its provider, OS and session type.
    enum class WindowFeature : std::uint32_t {
        AlwaysOnTop = 0, AspectRatio, MousePassthrough, Focusable, AutoMinimizeOnFocusLoss, ParentWindow, ModalWindow, KeyboardGrab,
        MouseConfineRect, WarpCursor, CustomCursor, FrameExtents, SafeArea, FullscreenTarget, TextInputOptions, ScreenKeyboard,
        ClipboardMimeTypes, TypedClipboard, HitTestRegions, SystemMenu, WindowIcon, RequestAttention, TaskbarProgress, SystemTheme,
        DisplayList, DisplayModes, DisplayEvents, LiveResizeCallback, WaitForEvents, FileDrop, TextDrop, WindowTypeHint, WindowShape,
        AnimatedCursor, KeyboardQuery, WarpCursorGlobal, MouseCapture, IccProfile, SystemTray, TouchInput, PenInput,
    };

    /// Mirrors `Engine::WindowOperation`: the key to `window_operation_status`.
    enum class WindowOperation : std::uint32_t {
        Title = 0, Position, Size, MinimumSize, MaximumSize, Resizable, Opacity, Maximize, Minimize, Restore, Raise, Focus, Show, Hide,
        AlwaysOnTop, AspectRatio, MousePassthrough, Focusable, AutoMinimize, Parent, KeyboardGrab, MouseConfineRect, CursorPosition,
        GlobalCursorPosition, MouseCapture, CustomCursor, AnimatedCursor, ClearCustomCursor, TextInputOptions, FullscreenTarget,
        WindowType, Shape, DisplayGamma, Icon, Attention, TaskbarProgress, HitTestRegions, SystemMenu, ClipboardText, SetClipboardText,
        ClipboardMimeTypes, ClipboardData, SetClipboardData, KeyName, KeyDown, IccProfile, FileDialog, MessageBox,
    };

    /// Mirrors `WindowManager::WindowType`.
    enum class WindowType : std::uint32_t { Normal = 0, Utility, Tooltip, PopupMenu, DropdownMenu, Dialog, Splash };

    /// The outcome of the last run of one operation on one window. `error` is 0 none, 1 unsupported (not a bug), 2 invalid
    /// argument, 3 failed.
    struct WindowOperationInfo {
        bool completed = false;
        bool ok = false;
        std::uint8_t error = 0;
        /// A query's boolean answer (`KeyDown`; whether `ClipboardData`'s format was present).
        bool flag = false;
        /// A numeric answer: the button a `MessageBox` returned (-1 when closed another way).
        std::int32_t number = 0;
    };

    /// Decoration thickness and safe area of a window, from the engine snapshot.
    struct WindowGeometryInfo {
        std::int32_t frame_left = 0;
        std::int32_t frame_top = 0;
        std::int32_t frame_right = 0;
        std::int32_t frame_bottom = 0;
        std::int32_t safe_x = 0;
        std::int32_t safe_y = 0;
        std::int32_t safe_width = 0;
        std::int32_t safe_height = 0;
        bool has_display = false;
        std::uint32_t display_id = 0;
    };

    /// A new window's configuration (the title is passed separately). Its graphics API always
    /// matches the engine's active backend.
    struct WindowDesc {
        /// Client-area size in physical pixels.
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        bool has_position = false;
        std::int32_t x = 0;
        std::int32_t y = 0;
        bool visible = true;
        bool resizable = true;
        bool decorated = true;
        bool high_dpi = true;
        bool transparent = false;
        WindowMode mode = WindowMode::Windowed;
    };

    enum class WindowRequestKind : std::uint32_t {
        Spawn = 0,
        Close = 1,
        RecreatePrimary = 2,
    };

    /// The outcome of a queued spawn/close/recreate request (the message is read separately).
    struct WindowRequestCompletion {
        std::uint64_t request_id = 0;
        WindowRequestKind kind = WindowRequestKind::Spawn;
        bool accepted = false;
        /// The window the request created or affected.
        std::uint64_t window_id = 0;
    };

} // namespace SFT::CxxApi
