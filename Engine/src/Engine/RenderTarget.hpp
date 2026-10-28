#pragma once

#include <Foundation/Foundation.hpp>

#include <string>
#include <vector>

namespace SFT::Engine {


    struct RenderTargetHandle {
        u64 value = 0;

        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return is_valid(); }

        friend constexpr bool operator==(RenderTargetHandle, RenderTargetHandle) noexcept = default;
    };

    /// What an off-screen target stores, and so what a frame writes into it.
    enum class RenderTargetFormat : u8 {
        /// Chosen from the target's `use` and what the device supports. The default.
        Auto,
        /// Display-encoded SDR, exactly what a window shows (tone mapped, clamped, sRGB-encoded).
        Bgra8Srgb,
        /// The same as `Bgra8Srgb` with RGBA channel order (for code that reads the pixels back and wants RGBA).
        Rgba8Srgb,
        /// Linear half-float, unclamped: 1.0 = paper white, highlights above it kept. For a target sampled back into an HDR scene (an
        /// emissive in-world screen, a portal, a probe) or processed further. Twice the memory of the 8-bit formats.
        Rgba16Float,
        /// Display-encoded SDR at 10 bits per channel (smoother gradients), 2-bit alpha. Encoded by a shader, so sampling returns
        /// sRGB-*encoded* values: decode them where linear light is needed. Same memory as the 8-bit formats.
        Rgb10A2,
    };

    /// What a target is for. Every setting left on Auto is decided from it, using the best the device offers for that use.
    enum class RenderTargetUse : u8 {
        /// Shown at about its own size (a UI image, a docked viewport): 8-bit display-encoded, no mips, no anisotropy. The default.
        Screen,
        /// Mapped onto world geometry and seen from afar and at angles (an in-world monitor, a portal): 8-bit display-encoded, the full
        /// mip chain, the device's highest anisotropy.
        World,
        /// Written by compute kernels: linear half-float with storage access when the device allows it.
        Compute,
    };

    /// How a target uses an optional capability.
    enum class RenderTargetFeature : u8 {
        /// Decided by the target's `use`.
        Auto,
        Off,
        /// On when the device (and the chosen format) supports it, otherwise off; never an error.
        IfSupported,
        /// On, or creation fails with the reason.
        Required,
    };

    enum class RenderTargetFilter : u8 {
        Linear,
        /// Pixel-exact (a pixel-art viewport, a retro monitor).
        Nearest,
    };

    /// What sampling outside [0, 1] returns.
    enum class RenderTargetAddressMode : u8 {
        ClampToEdge,
        Repeat,
        MirroredRepeat,
    };

    /// `OffscreenRenderTargetDescription::mip_levels` value meaning "decided by `use`".
    inline constexpr u32 render_target_auto_mips = ~0u;
    /// `RenderTargetSampling::max_anisotropy` value meaning "decided by `use`" (any negative value works).
    inline constexpr f32 render_target_auto_anisotropy = -1.0f;

    /// How materials (and custom passes) sample the target. UI images choose their own filtering per image (`ImageSampling`).
    struct RenderTargetSampling {
        RenderTargetFilter filter = RenderTargetFilter::Linear;
        RenderTargetAddressMode address = RenderTargetAddressMode::ClampToEdge;
        /// Anisotropic filtering for a target seen at a grazing angle: negative = Auto (by `use`), 0 or 1 = off, more = a request
        /// fitted to the device. Linear filter only.
        f32 max_anisotropy = render_target_auto_anisotropy;

        friend bool operator==(const RenderTargetSampling &, const RenderTargetSampling &) = default;
    };

    /// A request for an off-screen render target. Set only `width`, `height` and `use` to get the best the device can do for that use;
    /// set any other field to take that decision over. `Engine::offscreen_render_target_config` reports what was actually made. An
    /// explicit value the device cannot meet falls back to the closest thing that works (reported in the config's `adjustments` and
    /// logged) unless `strict`, which makes it an error. See `Renderer::OffscreenRenderTargetDescription` for the full contract.
    struct OffscreenRenderTargetDescription {
        u32 width = 0;
        u32 height = 0;
        UString label;
        RenderTargetFormat format = RenderTargetFormat::Auto;
        RenderTargetSampling sampling{};
        /// `render_target_auto_mips` = Auto; 1 = none; 0 = the full chain down to 1x1; otherwise a count. Rebuilt from level 0 after
        /// every frame aimed at the target.
        u32 mip_levels = render_target_auto_mips;
        /// Lets compute kernels write the target (`Engine::dispatch_into_offscreen_render_target`).
        RenderTargetFeature storage = RenderTargetFeature::Auto;
        /// 0 = each frame's own `anti_aliasing.msaa_samples`; 1, 2, 4, 8, 16 = this many for every frame aimed at the target.
        u32 msaa_samples = 0;
        RenderTargetUse use = RenderTargetUse::Screen;
        /// Fail rather than fall back when an explicit value is beyond the device.
        bool strict = false;
    };

    /// What an off-screen target request resolved to on the current device.
    struct RenderTargetConfig {
        u32 width = 0;
        u32 height = 0;
        /// Never `Auto`.
        RenderTargetFormat format = RenderTargetFormat::Bgra8Srgb;
        u32 mip_levels = 1;
        RenderTargetFilter filter = RenderTargetFilter::Linear;
        RenderTargetAddressMode address = RenderTargetAddressMode::ClampToEdge;
        /// 0 = off.
        f32 max_anisotropy = 0.0f;
        bool storage = false;
        /// 0 = the frame decides.
        u32 msaa_samples = 0;
        /// One line per explicit request the device could not meet and what was used instead.
        std::vector<std::string> adjustments;
    };

} // namespace SFT::Engine
