#pragma once

#include <Foundation/src/Foundation.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <graphicsPlatform/src/GraphicsPlatform.hpp>

#include "Swapchain.hpp"

using std::optional;
using std::string;
using std::string_view;
using std::vector;

namespace SFT::RHI {

    enum class HdrTransferFunction : u32 {
        Unknown,
        Sdr,
        PqSt2084,
        Hlg,
        LinearExtended,
    };

    enum class HdrColorGamut : u32 {
        Unknown,
        Rec709,
        DisplayP3,
        Rec2020,
    };

    enum class HdrMetadataSource : u32 {
        Unknown,
        GraphicsApi,
        OperatingSystem,
        WindowSystem,
        Edid,
        UserCalibration,
        EngineDefault,
    };

    enum class HdrMetadataConfidence : u32 {
        Unknown,
        Estimated,
        Reported,
        Calibrated,
        Measured,
    };

    struct Chromaticity {
        f32 x = 0.0f;
        f32 y = 0.0f;
    };

    struct HdrDisplayMetadata {
        Chromaticity red_primary{};
        Chromaticity green_primary{};
        Chromaticity blue_primary{};
        Chromaticity white_point{};

        f32 min_luminance_nits = 0.0f;
        f32 max_luminance_nits = 0.0f;
        f32 max_full_frame_luminance_nits = 0.0f;

        HdrMetadataSource source = HdrMetadataSource::Unknown;
        HdrMetadataConfidence confidence = HdrMetadataConfidence::Unknown;
    };

    // A "poor man's HDR10+" content-light-level refresh: HDR10+'s actual defining feature is dynamic
    // metadata standardized in SMPTE ST 2094-40, delivered per-scene/per-frame — Vulkan's
    // VK_EXT_hdr_metadata only exposes the *static* ST 2086 mastering-display metadata
    // vkSetHdrMetadataEXT sets once, and there is no portable Vulkan API for real ST 2094-40 delivery
    // (it isn't part of the Vulkan spec at all). This lets a caller re-call vkSetHdrMetadataEXT on an
    // already-live swapchain — legal per spec, no swapchain recreation needed — with updated
    // content-light-level numbers for the current scene, reusing the display's real mastering
    // primaries/white-point/luminance range captured at swapchain-creation time. It is NOT SMPTE ST
    // 2094-40 and will not be recognized as "HDR10+" by any receiver/certification test — it is only
    // ever as good as whatever `RhiDevice::update_hdr_content_light_level()`'s caller supplies (this
    // engine does not compute scene luminance statistics itself). Only meaningful for a swapchain
    // created with ColorSpace::Hdr10St2084 — HLG carries no metadata by design and scRGB/DolbyVision
    // don't use this metadata path either (DolbyVision's real dynamic metadata is an entirely
    // different, proprietary format this can't produce — see ColorSpace::DolbyVision's own doc
    // comment).
    struct HdrContentLightLevelUpdate {
        f32 max_content_light_level_nits = 0.0f;       // MaxCLL for the current scene/frame window.
        f32 max_frame_average_light_level_nits = 0.0f; // MaxFALL for the current scene/frame window.
    };

    struct HdrPresentationMode {
        HdrTransferFunction transfer = HdrTransferFunction::Unknown;
        HdrColorGamut gamut = HdrColorGamut::Unknown;
        bool requires_os_hdr_mode = true;
    };

    struct SurfaceHdrCapabilities {
        bool hdr_supported = false;
        bool hdr_enabled_by_os = false;
        bool hdr_metadata_output_supported = false;

        vector<HdrPresentationMode> supported_modes{};
        optional<HdrDisplayMetadata> display_metadata{};

        f32 sdr_white_nits = 80.0f;
        f32 edr_headroom = 1.0f;
        f32 max_edr_headroom = 1.0f;
    };

    struct DisplayInfo {
        string stable_id{};
        string name{};
        bool connected = false;
        bool primary = false;
        SurfaceHdrCapabilities hdr{};
    };

    enum class PlatformQueryStatus : u32 {
        Ok,
        Unsupported,
        NotAvailable,
        InvalidArgument,
        PlatformError,
    };

    struct PlatformQueryMessage {
        PlatformQueryStatus status = PlatformQueryStatus::Ok;
        string message{};

        [[nodiscard]] explicit operator bool() const noexcept;
    };

    struct SurfaceHdrCapabilityQuery {
        SurfaceHdrCapabilities capabilities{};
        PlatformQueryMessage message{};

        [[nodiscard]] explicit operator bool() const noexcept;
    };

    struct DisplayQuery {
        vector<DisplayInfo> displays{};
        PlatformQueryMessage message{};

        [[nodiscard]] explicit operator bool() const noexcept;
    };

    [[nodiscard]] GraphicsPlatform::WindowSystem to_graphics_platform(WindowSystem system) noexcept;

    [[nodiscard]] HdrTransferFunction to_rhi(GraphicsPlatform::HdrTransferFunction transfer) noexcept;

    [[nodiscard]] HdrColorGamut to_rhi(GraphicsPlatform::HdrColorGamut gamut) noexcept;

    [[nodiscard]] HdrMetadataSource to_rhi(GraphicsPlatform::HdrMetadataSource source) noexcept;

    [[nodiscard]] HdrMetadataConfidence to_rhi(GraphicsPlatform::HdrMetadataConfidence confidence) noexcept;

    [[nodiscard]] PlatformQueryStatus to_rhi(GraphicsPlatform::QueryStatus status) noexcept;

    [[nodiscard]] SurfaceHdrCapabilities to_rhi(const GraphicsPlatform::HdrDisplayCapabilities &capabilities);

    [[nodiscard]] DisplayInfo to_rhi(const GraphicsPlatform::DisplayInfo &display);

    [[nodiscard]] DisplayQuery query_platform_displays();

    [[nodiscard]] SurfaceHdrCapabilityQuery query_platform_hdr_display_capabilities(const SurfaceDesc &surface);

} // namespace SFT::RHI
