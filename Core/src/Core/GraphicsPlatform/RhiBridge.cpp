#include <Core/GraphicsPlatform/RhiBridge.hpp>

#include <string_view>

namespace SFT::Core {

    GraphicsPlatform::WindowSystem to_graphics_platform(RHI::WindowSystem system) noexcept {
        switch (system) {
            case RHI::WindowSystem::Unknown: return GraphicsPlatform::WindowSystem::Unknown;
            case RHI::WindowSystem::Win32: return GraphicsPlatform::WindowSystem::Win32;
            case RHI::WindowSystem::Xlib: return GraphicsPlatform::WindowSystem::Xlib;
            case RHI::WindowSystem::Xcb: return GraphicsPlatform::WindowSystem::Xcb;
            case RHI::WindowSystem::Wayland: return GraphicsPlatform::WindowSystem::Wayland;
            case RHI::WindowSystem::Cocoa: return GraphicsPlatform::WindowSystem::Cocoa;
            case RHI::WindowSystem::Android: return GraphicsPlatform::WindowSystem::Android;
            case RHI::WindowSystem::UIKit: return GraphicsPlatform::WindowSystem::UIKit;
            case RHI::WindowSystem::WebCanvas: return GraphicsPlatform::WindowSystem::WebCanvas;
        }
        return GraphicsPlatform::WindowSystem::Unknown;
    }

    RHI::HdrTransferFunction to_rhi(GraphicsPlatform::HdrTransferFunction transfer) noexcept {
        switch (transfer) {
            case GraphicsPlatform::HdrTransferFunction::Unknown: return RHI::HdrTransferFunction::Unknown;
            case GraphicsPlatform::HdrTransferFunction::Sdr: return RHI::HdrTransferFunction::Sdr;
            case GraphicsPlatform::HdrTransferFunction::PqSt2084: return RHI::HdrTransferFunction::PqSt2084;
            case GraphicsPlatform::HdrTransferFunction::Hlg: return RHI::HdrTransferFunction::Hlg;
            case GraphicsPlatform::HdrTransferFunction::LinearExtended: return RHI::HdrTransferFunction::LinearExtended;
        }
        return RHI::HdrTransferFunction::Unknown;
    }

    RHI::HdrColorGamut to_rhi(GraphicsPlatform::HdrColorGamut gamut) noexcept {
        switch (gamut) {
            case GraphicsPlatform::HdrColorGamut::Unknown: return RHI::HdrColorGamut::Unknown;
            case GraphicsPlatform::HdrColorGamut::Rec709: return RHI::HdrColorGamut::Rec709;
            case GraphicsPlatform::HdrColorGamut::DisplayP3: return RHI::HdrColorGamut::DisplayP3;
            case GraphicsPlatform::HdrColorGamut::Rec2020: return RHI::HdrColorGamut::Rec2020;
        }
        return RHI::HdrColorGamut::Unknown;
    }

    RHI::HdrMetadataSource to_rhi(GraphicsPlatform::HdrMetadataSource source) noexcept {
        switch (source) {
            case GraphicsPlatform::HdrMetadataSource::Unknown: return RHI::HdrMetadataSource::Unknown;
            case GraphicsPlatform::HdrMetadataSource::GraphicsApi: return RHI::HdrMetadataSource::GraphicsApi;
            case GraphicsPlatform::HdrMetadataSource::OperatingSystem: return RHI::HdrMetadataSource::OperatingSystem;
            case GraphicsPlatform::HdrMetadataSource::WindowSystem: return RHI::HdrMetadataSource::WindowSystem;
            case GraphicsPlatform::HdrMetadataSource::Edid: return RHI::HdrMetadataSource::Edid;
            case GraphicsPlatform::HdrMetadataSource::UserCalibration: return RHI::HdrMetadataSource::UserCalibration;
            case GraphicsPlatform::HdrMetadataSource::EngineDefault: return RHI::HdrMetadataSource::EngineDefault;
        }
        return RHI::HdrMetadataSource::Unknown;
    }

    RHI::HdrMetadataConfidence to_rhi(GraphicsPlatform::HdrMetadataConfidence confidence) noexcept {
        switch (confidence) {
            case GraphicsPlatform::HdrMetadataConfidence::Unknown: return RHI::HdrMetadataConfidence::Unknown;
            case GraphicsPlatform::HdrMetadataConfidence::Estimated: return RHI::HdrMetadataConfidence::Estimated;
            case GraphicsPlatform::HdrMetadataConfidence::Reported: return RHI::HdrMetadataConfidence::Reported;
            case GraphicsPlatform::HdrMetadataConfidence::Calibrated: return RHI::HdrMetadataConfidence::Calibrated;
            case GraphicsPlatform::HdrMetadataConfidence::Measured: return RHI::HdrMetadataConfidence::Measured;
        }
        return RHI::HdrMetadataConfidence::Unknown;
    }

    RHI::PlatformQueryStatus to_rhi(GraphicsPlatform::QueryStatus status) noexcept {
        switch (status) {
            case GraphicsPlatform::QueryStatus::Ok: return RHI::PlatformQueryStatus::Ok;
            case GraphicsPlatform::QueryStatus::Unsupported: return RHI::PlatformQueryStatus::Unsupported;
            case GraphicsPlatform::QueryStatus::NotAvailable: return RHI::PlatformQueryStatus::NotAvailable;
            case GraphicsPlatform::QueryStatus::InvalidArgument: return RHI::PlatformQueryStatus::InvalidArgument;
            case GraphicsPlatform::QueryStatus::PlatformError: return RHI::PlatformQueryStatus::PlatformError;
        }
        return RHI::PlatformQueryStatus::PlatformError;
    }

    RHI::SurfaceHdrCapabilities to_rhi(const GraphicsPlatform::HdrDisplayCapabilities &capabilities) {
        RHI::SurfaceHdrCapabilities out{
            .hdr_supported = capabilities.hdr_supported,
            .hdr_enabled_by_os = capabilities.hdr_enabled_by_os,
            .hdr_metadata_output_supported = capabilities.hdr_metadata_output_supported,
            .sdr_white_nits = capabilities.sdr_white_nits,
            .edr_headroom = capabilities.edr_headroom,
            .max_edr_headroom = capabilities.max_edr_headroom,
        };
        out.supported_modes.reserve(capabilities.supported_modes.size());
        for (const GraphicsPlatform::HdrPresentationMode &mode : capabilities.supported_modes) {
            out.supported_modes.push_back(RHI::HdrPresentationMode{
                .transfer = to_rhi(mode.transfer),
                .gamut = to_rhi(mode.gamut),
                .requires_os_hdr_mode = mode.requires_os_hdr_mode,
            });
        }
        if (capabilities.display_metadata.has_value()) {
            const GraphicsPlatform::HdrDisplayMetadata &metadata = *capabilities.display_metadata;
            out.display_metadata = RHI::HdrDisplayMetadata{
                .red_primary = RHI::Chromaticity{.x = metadata.red_primary.x, .y = metadata.red_primary.y},
                .green_primary = RHI::Chromaticity{.x = metadata.green_primary.x, .y = metadata.green_primary.y},
                .blue_primary = RHI::Chromaticity{.x = metadata.blue_primary.x, .y = metadata.blue_primary.y},
                .white_point = RHI::Chromaticity{.x = metadata.white_point.x, .y = metadata.white_point.y},
                .min_luminance_nits = metadata.min_luminance_nits,
                .max_luminance_nits = metadata.max_luminance_nits,
                .max_full_frame_luminance_nits = metadata.max_full_frame_luminance_nits,
                .source = to_rhi(metadata.source),
                .confidence = to_rhi(metadata.confidence),
            };
        }
        return out;
    }

    RHI::DisplayInfo to_rhi(const GraphicsPlatform::DisplayInfo &display) {
        return RHI::DisplayInfo{
            .stable_id = display.stable_id,
            .name = display.name,
            .connected = display.connected,
            .primary = display.primary,
            .hdr = to_rhi(display.hdr),
        };
    }

    RHI::DisplayQuery query_platform_displays() {
        const auto query = GraphicsPlatform::query_displays();
        RHI::DisplayQuery out{
            .message = RHI::PlatformQueryMessage{
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

    RHI::SurfaceHdrCapabilityQuery query_platform_hdr_display_capabilities(const RHI::SurfaceDesc &surface) {
        const GraphicsPlatform::NativeSurfaceHandle platform_surface{
            .system = to_graphics_platform(surface.system),
            .display = surface.display,
            .window = surface.window,
            .label = surface.label != nullptr ? std::string_view{surface.label} : std::string_view{},
        };
        const auto query = GraphicsPlatform::query_hdr_display_capabilities(platform_surface);
        return RHI::SurfaceHdrCapabilityQuery{
            .capabilities = to_rhi(query.value),
            .message = RHI::PlatformQueryMessage{
                .status = to_rhi(query.message.status),
                .message = query.message.message,
            },
        };
    }

} // namespace SFT::Core
