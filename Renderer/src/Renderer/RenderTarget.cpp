#include <Renderer/RenderTarget.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <span>

namespace SFT::Renderer {

    namespace {

        [[nodiscard]] const char *format_name(RHI::Format format) noexcept {
            switch (format) {
                case RHI::Format::BGRA8UnormSrgb: return "BGRA8UnormSrgb";
                case RHI::Format::RGBA8UnormSrgb: return "RGBA8UnormSrgb";
                case RHI::Format::RGB10A2Unorm: return "RGB10A2Unorm";
                case RHI::Format::RGBA16Float: return "RGBA16Float";
                default: return "an unsupported format";
            }
        }

        /// Everything a target needs from its format whatever it is used for: drawn into (with blending, for overlays), sampled with
        /// filtering (materials, and the mip filter's texel-centre taps).
        [[nodiscard]] bool usable_as_target(const RHI::FormatCapabilities &capabilities) noexcept {
            return capabilities.sampled && capabilities.filterable && capabilities.color_attachment && capabilities.blendable;
        }

        /// What to try, in order, when a format the caller named is beyond the device: the nearest in precision first, then the
        /// universal 8-bit formats.
        [[nodiscard]] std::span<const RHI::Format> fallbacks_for(RHI::Format format) noexcept {
            static constexpr std::array ten_bit{RHI::Format::RGBA16Float, RHI::Format::BGRA8UnormSrgb, RHI::Format::RGBA8UnormSrgb};
            static constexpr std::array half_float{RHI::Format::BGRA8UnormSrgb, RHI::Format::RGBA8UnormSrgb};
            static constexpr std::array rgba8{RHI::Format::BGRA8UnormSrgb};
            static constexpr std::array bgra8{RHI::Format::RGBA8UnormSrgb};
            switch (format) {
                case RHI::Format::RGB10A2Unorm: return ten_bit;
                case RHI::Format::RGBA16Float: return half_float;
                case RHI::Format::RGBA8UnormSrgb: return rgba8;
                case RHI::Format::BGRA8UnormSrgb: return bgra8;
                default: return {};
            }
        }

        /// The formats an Auto target tries, best first, for its use.
        [[nodiscard]] std::span<const RHI::Format> auto_formats_for(OffscreenRenderTargetUse use) noexcept {
            static constexpr std::array display{RHI::Format::BGRA8UnormSrgb, RHI::Format::RGBA8UnormSrgb, RHI::Format::RGBA16Float};
            static constexpr std::array compute{RHI::Format::RGBA16Float, RHI::Format::BGRA8UnormSrgb, RHI::Format::RGBA8UnormSrgb};
            return use == OffscreenRenderTargetUse::Compute ? std::span<const RHI::Format>{compute} : std::span<const RHI::Format>{display};
        }

    } // namespace

    std::expected<ResolvedOffscreenRenderTargetConfig, std::string> resolve_offscreen_render_target(
        const OffscreenRenderTargetDescription &request, const OffscreenTargetDeviceCapabilities &device) {
        ResolvedOffscreenRenderTargetConfig out;
        const auto capabilities = [&device](RHI::Format format) {
            return device.format_capabilities ? device.format_capabilities(format) : RHI::portable_format_capabilities(format);
        };

        // ---- size ----
        if (request.extent.x == 0 || request.extent.y == 0) {
            return std::unexpected(std::string{"Off-screen render targets require a non-zero extent."});
        }
        out.extent = request.extent;
        const u32 max_dimension = device.limits.max_texture_dimension_2d;
        if (max_dimension != 0 && (request.extent.x > max_dimension || request.extent.y > max_dimension)) {
            if (request.strict) {
                return std::unexpected(std::format("{}x{} is larger than the device's maximum texture size ({}).", request.extent.x,
                                                   request.extent.y, max_dimension));
            }
            const f64 scale = static_cast<f64>(max_dimension) / static_cast<f64>(std::max(request.extent.x, request.extent.y));
            out.extent = Core::Extent2D{std::clamp(static_cast<u32>(std::lround(request.extent.x * scale)), 1u, max_dimension),
                                        std::clamp(static_cast<u32>(std::lround(request.extent.y * scale)), 1u, max_dimension)};
            out.adjustments.push_back(std::format("size {}x{} exceeds the device maximum {}; using {}x{}", request.extent.x,
                                                  request.extent.y, max_dimension, out.extent.x, out.extent.y));
        }

        // ---- storage wish (it steers an Auto format) ----
        const OffscreenTargetFeature storage =
            request.storage != OffscreenTargetFeature::Auto ? request.storage
            : request.use == OffscreenRenderTargetUse::Compute ? OffscreenTargetFeature::IfSupported
                                                               : OffscreenTargetFeature::Off;
        const bool wants_storage = storage == OffscreenTargetFeature::IfSupported || storage == OffscreenTargetFeature::Required;

        // ---- format ----
        if (request.format == RHI::Format::Undefined) {
            const std::span<const RHI::Format> candidates = auto_formats_for(request.use);
            const auto pick = [&](bool with_storage) -> std::optional<RHI::Format> {
                for (const RHI::Format candidate : candidates) {
                    const RHI::FormatCapabilities caps = capabilities(candidate);
                    if (usable_as_target(caps) && (!with_storage || caps.storage)) {
                        return candidate;
                    }
                }
                return std::nullopt;
            };
            std::optional<RHI::Format> chosen = wants_storage ? pick(true) : std::nullopt;
            if (!chosen) {
                chosen = pick(false);
            }
            if (!chosen) {
                return std::unexpected(std::string{"The device supports none of the off-screen render target formats."});
            }
            out.format = *chosen;
        } else {
            if (!is_supported_offscreen_render_target_format(request.format)) {
                return std::unexpected(std::string{
                    "Unsupported off-screen render target format; use BGRA8UnormSrgb, RGBA8UnormSrgb, RGB10A2Unorm (display-encoded SDR) "
                    "or RGBA16Float (linear HDR)."});
            }
            if (usable_as_target(capabilities(request.format))) {
                out.format = request.format;
            } else {
                if (request.strict) {
                    return std::unexpected(std::format("The device cannot render, blend and filter {}.", format_name(request.format)));
                }
                const std::span<const RHI::Format> fallbacks = fallbacks_for(request.format);
                const auto found = std::ranges::find_if(fallbacks, [&](RHI::Format f) { return usable_as_target(capabilities(f)); });
                if (found == fallbacks.end()) {
                    return std::unexpected(std::format("The device cannot render {} and has no fallback format.", format_name(request.format)));
                }
                out.format = *found;
                out.adjustments.push_back(std::format("format {} is not renderable/blendable/filterable here; using {}",
                                                      format_name(request.format), format_name(out.format)));
            }
        }

        // ---- storage ----
        if (wants_storage) {
            if (capabilities(out.format).storage) {
                out.storage = true;
            } else if (storage == OffscreenTargetFeature::Required) {
                return std::unexpected(std::format("{} cannot be a storage image on this device; storage was required (pick a format "
                                                   "that can, such as RGBA16Float, or leave the format on Auto).",
                                                   format_name(out.format)));
            } else if (request.storage == OffscreenTargetFeature::IfSupported) {
                out.adjustments.push_back(std::format("storage is not available for {} here; the target has none", format_name(out.format)));
            }
        }

        // ---- mips ----
        const u32 full_chain = full_mip_chain_length(out.extent);
        const u32 requested_mips = request.mip_levels != offscreen_target_auto_mips ? request.mip_levels
                                   : request.use == OffscreenRenderTargetUse::World ? 0u
                                                                                    : 1u;
        out.mip_levels = requested_mips == 0 ? full_chain : std::min(requested_mips, full_chain);

        // ---- sampling ----
        out.filter = request.sampling.filter;
        out.address = request.sampling.address;
        const f32 device_anisotropy = std::min(device.limits.max_sampler_anisotropy, 16.0f);
        const f32 anisotropy = request.sampling.max_anisotropy;
        if (anisotropy < 0.0f) {
            // Auto: the most the device offers for a target seen at angles; nothing otherwise.
            out.max_anisotropy = request.use == OffscreenRenderTargetUse::World && out.filter == RHI::Filter::Linear && device_anisotropy > 1.0f
                                     ? device_anisotropy
                                     : 0.0f;
        } else if (anisotropy > 1.0f) {
            if (out.filter != RHI::Filter::Linear) {
                if (request.strict) {
                    return std::unexpected(std::string{"Anisotropic filtering needs the linear filter."});
                }
                out.adjustments.push_back("anisotropic filtering needs the linear filter; anisotropy off");
            } else if (device_anisotropy <= 1.0f) {
                if (request.strict) {
                    return std::unexpected(std::string{"The device has no anisotropic filtering."});
                }
                out.adjustments.push_back("the device has no anisotropic filtering; anisotropy off");
            } else if (anisotropy > device_anisotropy) {
                if (request.strict) {
                    return std::unexpected(std::format("Anisotropy {} exceeds the device maximum {}.", anisotropy, device_anisotropy));
                }
                out.max_anisotropy = device_anisotropy;
                out.adjustments.push_back(std::format("anisotropy {} exceeds the device maximum; using {}", anisotropy, device_anisotropy));
            } else {
                out.max_anisotropy = anisotropy;
            }
        }

        // ---- multisampling ----
        if (request.msaa_samples != 0) {
            const u32 samples = request.msaa_samples;
            if (samples > 16 || (samples & (samples - 1)) != 0) {
                return std::unexpected(std::format("msaa_samples must be 1, 2, 4, 8 or 16 (got {}).", samples));
            }
            // The scene is what is multisampled (its deferred targets), so the device's framebuffer sample counts decide.
            const u32 supported = device.limits.framebuffer_sample_counts | 1u;
            u32 chosen = samples;
            while (chosen > 1 && (supported & chosen) == 0) {
                chosen >>= 1;
            }
            if (chosen != samples) {
                if (request.strict) {
                    return std::unexpected(std::format("The device cannot multisample {}x.", samples));
                }
                out.adjustments.push_back(std::format("{}x multisampling is not supported; using {}x", samples, chosen));
            }
            out.msaa_samples = chosen;
        }
        return out;
    }

} // namespace SFT::Renderer
