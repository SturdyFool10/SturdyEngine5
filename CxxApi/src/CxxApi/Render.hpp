#pragma once

#include <CxxApi/Runtime.hpp>
#include <CxxApi/Types/Render.hpp>

namespace SFT::CxxApi {

    /// Every render setting at its engine default (the Rust `Default` for `FrameSettings`).
    [[nodiscard]] FrameSettings frame_settings_defaults() noexcept;
    /// This frame's render settings as they stand (defaults, or whatever was set earlier this frame).
    [[nodiscard]] FrameSettings frame_render_settings(const FrameBuilder &frame) noexcept;
    /// Replaces this frame's render settings wholesale. Read-modify-write with `frame_render_settings` to change one field.
    void frame_set_render_settings(FrameBuilder &frame, const FrameSettings &settings) noexcept;

} // namespace SFT::CxxApi
