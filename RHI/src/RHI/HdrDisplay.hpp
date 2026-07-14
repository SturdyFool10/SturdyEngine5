#pragma once

#include <Foundation/Foundation.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <graphicsPlatform/GraphicsPlatform.hpp>

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

        [[nodiscard]] explicit operator bool() const noexcept { return status == PlatformQueryStatus::Ok; }
    };

    struct SurfaceHdrCapabilityQuery {
        SurfaceHdrCapabilities capabilities{};
        PlatformQueryMessage message{};

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(message); }
    };

    struct DisplayQuery {
        vector<DisplayInfo> displays{};
        PlatformQueryMessage message{};

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(message); }
    };

    [[nodiscard]] inline GraphicsPlatform::WindowSystem to_graphics_platform(WindowSystem system) noexcept {
        switch (system) {
            case WindowSystem::Unknown: return GraphicsPlatform::WindowSystem::Unknown;
            case WindowSystem::Win32: return GraphicsPlatform::WindowSystem::Win32;
            case WindowSystem::Xlib: return GraphicsPlatform::WindowSystem::Xlib;
            case WindowSystem::Xcb: return GraphicsPlatform::WindowSystem::Xcb;
            case WindowSystem::Wayland: return GraphicsPlatform::WindowSystem::Wayland;
            case WindowSystem::Cocoa: return GraphicsPlatform::WindowSystem::Cocoa;
            case WindowSystem::Android: return GraphicsPlatform::WindowSystem::Android;
            case WindowSystem::UIKit: return GraphicsPlatform::WindowSystem::UIKit;
        }
        return GraphicsPlatform::WindowSystem::Unknown;
    }

    [[nodiscard]] inline HdrTransferFunction to_rhi(GraphicsPlatform::HdrTransferFunction transfer) noexcept {
        switch (transfer) {
            case GraphicsPlatform::HdrTransferFunction::Unknown: return HdrTransferFunction::Unknown;
            case GraphicsPlatform::HdrTransferFunction::Sdr: return HdrTransferFunction::Sdr;
            case GraphicsPlatform::HdrTransferFunction::PqSt2084: return HdrTransferFunction::PqSt2084;
            case GraphicsPlatform::HdrTransferFunction::Hlg: return HdrTransferFunction::Hlg;
            case GraphicsPlatform::HdrTransferFunction::LinearExtended: return HdrTransferFunction::LinearExtended;
        }
        return HdrTransferFunction::Unknown;
    }

    [[nodiscard]] inline HdrColorGamut to_rhi(GraphicsPlatform::HdrColorGamut gamut) noexcept {
        switch (gamut) {
            case GraphicsPlatform::HdrColorGamut::Unknown: return HdrColorGamut::Unknown;
            case GraphicsPlatform::HdrColorGamut::Rec709: return HdrColorGamut::Rec709;
            case GraphicsPlatform::HdrColorGamut::DisplayP3: return HdrColorGamut::DisplayP3;
            case GraphicsPlatform::HdrColorGamut::Rec2020: return HdrColorGamut::Rec2020;
        }
        return HdrColorGamut::Unknown;
    }

    [[nodiscard]] inline HdrMetadataSource to_rhi(GraphicsPlatform::HdrMetadataSource source) noexcept {
        switch (source) {
            case GraphicsPlatform::HdrMetadataSource::Unknown: return HdrMetadataSource::Unknown;
            case GraphicsPlatform::HdrMetadataSource::GraphicsApi: return HdrMetadataSource::GraphicsApi;
            case GraphicsPlatform::HdrMetadataSource::OperatingSystem: return HdrMetadataSource::OperatingSystem;
            case GraphicsPlatform::HdrMetadataSource::WindowSystem: return HdrMetadataSource::WindowSystem;
            case GraphicsPlatform::HdrMetadataSource::Edid: return HdrMetadataSource::Edid;
            case GraphicsPlatform::HdrMetadataSource::UserCalibration: return HdrMetadataSource::UserCalibration;
            case GraphicsPlatform::HdrMetadataSource::EngineDefault: return HdrMetadataSource::EngineDefault;
        }
        return HdrMetadataSource::Unknown;
    }

    [[nodiscard]] inline HdrMetadataConfidence to_rhi(GraphicsPlatform::HdrMetadataConfidence confidence) noexcept {
        switch (confidence) {
            case GraphicsPlatform::HdrMetadataConfidence::Unknown: return HdrMetadataConfidence::Unknown;
            case GraphicsPlatform::HdrMetadataConfidence::Estimated: return HdrMetadataConfidence::Estimated;
            case GraphicsPlatform::HdrMetadataConfidence::Reported: return HdrMetadataConfidence::Reported;
            case GraphicsPlatform::HdrMetadataConfidence::Calibrated: return HdrMetadataConfidence::Calibrated;
            case GraphicsPlatform::HdrMetadataConfidence::Measured: return HdrMetadataConfidence::Measured;
        }
        return HdrMetadataConfidence::Unknown;
    }

    [[nodiscard]] inline PlatformQueryStatus to_rhi(GraphicsPlatform::QueryStatus status) noexcept {
        switch (status) {
            case GraphicsPlatform::QueryStatus::Ok: return PlatformQueryStatus::Ok;
            case GraphicsPlatform::QueryStatus::Unsupported: return PlatformQueryStatus::Unsupported;
            case GraphicsPlatform::QueryStatus::NotAvailable: return PlatformQueryStatus::NotAvailable;
            case GraphicsPlatform::QueryStatus::InvalidArgument: return PlatformQueryStatus::InvalidArgument;
            case GraphicsPlatform::QueryStatus::PlatformError: return PlatformQueryStatus::PlatformError;
        }
        return PlatformQueryStatus::PlatformError;
    }

    [[nodiscard]] inline SurfaceHdrCapabilities to_rhi(const GraphicsPlatform::HdrDisplayCapabilities &capabilities) {
        SurfaceHdrCapabilities out{
            .hdr_supported = capabilities.hdr_supported,
            .hdr_enabled_by_os = capabilities.hdr_enabled_by_os,
            .hdr_metadata_output_supported = capabilities.hdr_metadata_output_supported,
            .sdr_white_nits = capabilities.sdr_white_nits,
            .edr_headroom = capabilities.edr_headroom,
            .max_edr_headroom = capabilities.max_edr_headroom,
        };
        out.supported_modes.reserve(capabilities.supported_modes.size());
        for (const GraphicsPlatform::HdrPresentationMode &mode : capabilities.supported_modes) {
            out.supported_modes.push_back(HdrPresentationMode{
                .transfer = to_rhi(mode.transfer),
                .gamut = to_rhi(mode.gamut),
                .requires_os_hdr_mode = mode.requires_os_hdr_mode,
            });
        }
        if (capabilities.display_metadata.has_value()) {
            const GraphicsPlatform::HdrDisplayMetadata &metadata = *capabilities.display_metadata;
            out.display_metadata = HdrDisplayMetadata{
                .red_primary = Chromaticity{.x = metadata.red_primary.x, .y = metadata.red_primary.y},
                .green_primary = Chromaticity{.x = metadata.green_primary.x, .y = metadata.green_primary.y},
                .blue_primary = Chromaticity{.x = metadata.blue_primary.x, .y = metadata.blue_primary.y},
                .white_point = Chromaticity{.x = metadata.white_point.x, .y = metadata.white_point.y},
                .min_luminance_nits = metadata.min_luminance_nits,
                .max_luminance_nits = metadata.max_luminance_nits,
                .max_full_frame_luminance_nits = metadata.max_full_frame_luminance_nits,
                .source = to_rhi(metadata.source),
                .confidence = to_rhi(metadata.confidence),
            };
        }
        return out;
    }

    [[nodiscard]] inline DisplayInfo to_rhi(const GraphicsPlatform::DisplayInfo &display) {
        return DisplayInfo{
            .stable_id = display.stable_id,
            .name = display.name,
            .connected = display.connected,
            .primary = display.primary,
            .hdr = to_rhi(display.hdr),
        };
    }

    [[nodiscard]] inline DisplayQuery query_platform_displays() {
        const GraphicsPlatform::QueryResult<vector<GraphicsPlatform::DisplayInfo>> query =
            GraphicsPlatform::query_displays();
        DisplayQuery out{
            .message = PlatformQueryMessage{
                .status = to_rhi(query.message.status),
                .message = query.message.message,
            },
        };
        out.displays.reserve(query.value.size());
        for (const GraphicsPlatform::DisplayInfo &display : query.value) {
            out.displays.push_back(to_rhi(display));
        }
        return out;
    }

    [[nodiscard]] inline SurfaceHdrCapabilityQuery query_platform_hdr_display_capabilities(const SurfaceDesc &surface) {
        const GraphicsPlatform::NativeSurfaceHandle platform_surface{
            .system = to_graphics_platform(surface.system),
            .display = surface.display,
            .window = surface.window,
            .label = surface.label != nullptr ? string_view{surface.label} : string_view{},
        };
        const GraphicsPlatform::QueryResult<GraphicsPlatform::HdrDisplayCapabilities> query =
            GraphicsPlatform::query_hdr_display_capabilities(platform_surface);
        return SurfaceHdrCapabilityQuery{
            .capabilities = to_rhi(query.value),
            .message = PlatformQueryMessage{
                .status = to_rhi(query.message.status),
                .message = query.message.message,
            },
        };
    }

} // namespace SFT::RHI
