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
