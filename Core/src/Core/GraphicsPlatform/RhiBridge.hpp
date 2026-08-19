#pragma once

#include <Core/GraphicsPlatform/GraphicsPlatform.hpp>
#include <RHI/RHI.hpp>

namespace SFT::Core {

    [[nodiscard]] GraphicsPlatform::WindowSystem to_graphics_platform(RHI::WindowSystem system) noexcept;
    [[nodiscard]] RHI::HdrTransferFunction to_rhi(GraphicsPlatform::HdrTransferFunction transfer) noexcept;
    [[nodiscard]] RHI::HdrColorGamut to_rhi(GraphicsPlatform::HdrColorGamut gamut) noexcept;
    [[nodiscard]] RHI::HdrMetadataSource to_rhi(GraphicsPlatform::HdrMetadataSource source) noexcept;
    [[nodiscard]] RHI::HdrMetadataConfidence to_rhi(GraphicsPlatform::HdrMetadataConfidence confidence) noexcept;
    [[nodiscard]] RHI::PlatformQueryStatus to_rhi(GraphicsPlatform::QueryStatus status) noexcept;
    [[nodiscard]] RHI::SurfaceHdrCapabilities to_rhi(const GraphicsPlatform::HdrDisplayCapabilities &capabilities);
    [[nodiscard]] RHI::DisplayInfo to_rhi(const GraphicsPlatform::DisplayInfo &display);
    [[nodiscard]] RHI::DisplayQuery query_platform_displays();
    [[nodiscard]] RHI::SurfaceHdrCapabilityQuery query_platform_hdr_display_capabilities(const RHI::SurfaceDesc &surface);

} // namespace SFT::Core
