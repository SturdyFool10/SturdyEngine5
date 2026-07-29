#pragma once

#include <Foundation/src/Foundation.hpp>

namespace SFT::Engine {

    // Stable Renderer-owned offscreen-target identity. Unlike render-graph-local handles, this value
    // has no graph generation and must survive graph/prepared-frame copies unchanged. Zero is invalid
    // and selects the frame's window surface when used by RenderModules::Present.
    struct RenderTargetHandle {
        u64 value = 0;

        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }

        friend constexpr bool operator==(RenderTargetHandle, RenderTargetHandle) noexcept = default;
    };

    // Absolute pixel size for a persistent Renderer-owned offscreen target. The initial target format
    // is intentionally fixed to SDR sRGB; format selection is omitted until the Engine can expose a
    // broader color-space/format contract without leaking RHI details into CPU frame snapshots.
    struct OffscreenRenderTargetDescription {
        u32 width = 0;
        u32 height = 0;
        UString label;
    };

} // namespace SFT::Engine
