#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <functional>
#include <string>
#include <vector>

#include <Core/Core.hpp>
#include <RHI/Device.hpp>
#include <RHI/Resources.hpp>
#include <RHI/Types.hpp>

#include <Renderer/Handles.hpp>

namespace SFT::Renderer {

    using OffscreenRenderTargetHandle = Handle<struct OffscreenRenderTargetTag>;

    /// What a target is for. It decides every setting left on Auto, so a target that names its use and nothing else gets the best
    /// configuration the device can do for that use.
    enum class OffscreenRenderTargetUse : u8 {
        /// Shown at about its own size: a UI image, a docked viewport, a thumbnail. Display-encoded 8-bit, no mips, no anisotropy.
        Screen,
        /// Mapped onto geometry in the world and seen from afar and at angles: an in-world monitor, a portal, a mirror. Display-encoded
        /// 8-bit, the full mip chain, and the device's highest anisotropy (no shimmer at distance, sharp at grazing angles).
        World,
        /// Written or read by compute shaders: linear half-float with storage access when the device allows it.
        Compute,
    };

    /// How a target uses an optional capability.
    enum class OffscreenTargetFeature : u8 {
        /// The target's `use` decides.
        Auto,
        Off,
        /// On when the device (and the chosen format) supports it, otherwise off. Never an error.
        IfSupported,
        /// On, or creation fails with the reason. Independent of `strict`.
        Required,
    };

    /// `mip_levels` value meaning "decided by `use`".
    inline constexpr u32 offscreen_target_auto_mips = ~0u;
    /// `max_anisotropy` value meaning "decided by `use`" (any negative value works).
    inline constexpr f32 offscreen_target_auto_anisotropy = -1.0f;

    /// How materials and custom passes sample an off-screen target. UI images choose their own filtering per image.
    struct OffscreenRenderTargetSampling {
        /// Magnification and minification filter. `Nearest` for pixel-exact display (a pixel-art viewport, a retro monitor).
        RHI::Filter filter = RHI::Filter::Linear;
        /// What a lookup outside [0, 1] returns. `Repeat` / `MirroredRepeat` tile the target (a texture painted live).
        RHI::AddressMode address = RHI::AddressMode::ClampToEdge;
        /// Anisotropic filtering for a target seen at a grazing angle. Negative = Auto (by `use`); 0 or 1 = off; above that, a
        /// request clamped to the device's limit. Linear filter only.
        f32 max_anisotropy = offscreen_target_auto_anisotropy;

        friend bool operator==(const OffscreenRenderTargetSampling &, const OffscreenRenderTargetSampling &) = default;
    };

    /// A request for an off-screen render target: a texture a frame renders into instead of a window, and that can then be sampled.
    /// Leave everything on Auto and set `use` to get the best the device can do for it; set any field to take that decision over.
    /// What the device actually gave is `ResolvedOffscreenRenderTargetConfig` (`Renderer::offscreen_render_target_config`), and it is
    /// worked out again for each device, so a target moved to a better GPU (a device switch) picks up what that GPU can do.
    ///
    /// **Explicit values on a device that cannot do them**: with `strict` off (the default) the target falls back to the closest
    /// thing that works and the config says what changed (`adjustments`, also logged once); with `strict` on, creation fails with the
    /// reason instead. `OffscreenTargetFeature::Required` fails regardless.
    ///
    /// **Formats** (anything else is rejected with an error naming these):
    /// - `BGRA8UnormSrgb`, `RGBA8UnormSrgb`: display-encoded SDR, the same pixels a window shows (tone mapped, clamped to
    ///   [0, 1], sRGB-encoded on write). Sampling returns linear values.
    /// - `RGB10A2Unorm`: display-encoded SDR at 10 bits per channel (less banding in gradients), 2-bit alpha. The values are
    ///   sRGB-encoded by a shader rather than by the format, so sampling returns *encoded* values: decode them (`srgbToLinear`)
    ///   where linear light is needed. The frame renders into a half-float intermediate that is encoded into the target at the end.
    /// - `RGBA16Float`: linear and unclamped, in the reference domain (1.0 = paper white; brighter highlights keep their value). For a
    ///   target sampled back into an HDR scene (an emissive in-world screen, a portal, a reflection probe) or processed further.
    ///
    /// **Mip levels**: with more than one, every frame aimed at the target (and every compute write into it) ends by filtering the
    /// chain down from level 0. 0 = the full chain.
    ///
    /// **Storage** lets a compute kernel write the target (`Renderer::dispatch_into_offscreen_render_target`). The sRGB formats can
    /// never be storage images; whether the others can depends on the device. Storage is not free (some GPUs turn off framebuffer
    /// compression for storage images), which is why only `Compute` targets get it automatically.
    ///
    /// **Multisampling** of the scene is normally the frame's (`anti_aliasing.msaa_samples`). `msaa_samples` here, when non-zero,
    /// overrides it for every frame aimed at this target, clamped to what the device supports for the scene's targets. UI overlays are
    /// anti-aliased analytically and do not need it.
    struct OffscreenRenderTargetDescription {
        /// Pixels. Larger than the device's maximum texture size is clamped (aspect kept) unless `strict`.
        Core::Extent2D extent{};
        /// `Undefined` = Auto (by `use`).
        RHI::Format format = RHI::Format::Undefined;
        OffscreenRenderTargetSampling sampling{};
        UString label;
        /// `offscreen_target_auto_mips` = Auto (by `use`); 0 = the full chain; otherwise a count, capped at the full chain.
        u32 mip_levels = offscreen_target_auto_mips;
        OffscreenTargetFeature storage = OffscreenTargetFeature::Auto;
        /// 0 = the frame decides; 1, 2, 4, 8, 16 = this many samples for frames aimed at this target.
        u32 msaa_samples = 0;
        OffscreenRenderTargetUse use = OffscreenRenderTargetUse::Screen;
        /// Fail rather than fall back when an explicit value is beyond the device.
        bool strict = false;
    };

    /// What a target request resolved to on the device it lives on.
    struct ResolvedOffscreenRenderTargetConfig {
        Core::Extent2D extent{};
        RHI::Format format = RHI::Format::BGRA8UnormSrgb;
        u32 mip_levels = 1;
        RHI::Filter filter = RHI::Filter::Linear;
        RHI::AddressMode address = RHI::AddressMode::ClampToEdge;
        /// 0 = off.
        f32 max_anisotropy = 0.0f;
        bool storage = false;
        /// 0 = the frame decides.
        u32 msaa_samples = 0;
        /// One line per explicit request the device could not meet and what was used instead. Empty when everything was as asked.
        std::vector<std::string> adjustments;

        /// Whether two configs need different GPU images (the sampling-only fields excluded).
        [[nodiscard]] bool same_image_as(const ResolvedOffscreenRenderTargetConfig &other) const noexcept {
            return extent == other.extent && format == other.format && mip_levels == other.mip_levels && storage == other.storage;
        }
    };

    /// What a device offers that target resolution depends on. `Renderer` fills it from its RHI device; tests can fake it.
    struct OffscreenTargetDeviceCapabilities {
        RHI::DeviceLimits limits{};
        std::function<RHI::FormatCapabilities(RHI::Format)> format_capabilities = RHI::portable_format_capabilities;
    };

    /// Works out what `request` gets on a device with `device`'s capabilities: Auto values from `use`, explicit values checked and
    /// (unless `strict`) degraded to what works. Fails for a zero extent, a format a target can never have, a `strict` request the
    /// device cannot meet, or a `Required` feature it cannot provide. Pure: no GPU objects are touched.
    [[nodiscard]] std::expected<ResolvedOffscreenRenderTargetConfig, std::string> resolve_offscreen_render_target(
        const OffscreenRenderTargetDescription &request, const OffscreenTargetDeviceCapabilities &device);

    /// Whether `format` can be an off-screen render target's format at all (on some device).
    [[nodiscard]] constexpr bool is_supported_offscreen_render_target_format(RHI::Format format) noexcept {
        return format == RHI::Format::BGRA8UnormSrgb || format == RHI::Format::RGBA8UnormSrgb || format == RHI::Format::RGB10A2Unorm ||
               format == RHI::Format::RGBA16Float;
    }

    /// Whether a frame writes linear, unclamped values into a target of `format` (rather than display-encoded SDR).
    [[nodiscard]] constexpr bool offscreen_render_target_format_is_linear(RHI::Format format) noexcept {
        return format == RHI::Format::RGBA16Float;
    }

    /// Whether a target of `format` stores sRGB-encoded values the format does not encode itself, so the frame renders into a linear
    /// intermediate and a shader encodes it into the target.
    [[nodiscard]] constexpr bool offscreen_render_target_format_needs_shader_encode(RHI::Format format) noexcept {
        return format == RHI::Format::RGB10A2Unorm;
    }

    /// Levels in a full mip chain for `extent` (down to 1x1).
    [[nodiscard]] constexpr u32 full_mip_chain_length(Core::Extent2D extent) noexcept {
        u32 levels = 1;
        for (u32 size = extent.x > extent.y ? extent.x : extent.y; size > 1; size >>= 1) {
            ++levels;
        }
        return levels;
    }

} // namespace SFT::Renderer
