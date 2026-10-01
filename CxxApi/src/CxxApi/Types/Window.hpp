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
