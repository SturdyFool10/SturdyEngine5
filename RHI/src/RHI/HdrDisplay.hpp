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


    struct HdrContentLightLevelUpdate {
        f32 max_content_light_level_nits = 0.0f;
        f32 max_frame_average_light_level_nits = 0.0f;
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

        /// Converts the `PlatformQueryMessage` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;
    };

    struct SurfaceHdrCapabilityQuery {
        SurfaceHdrCapabilities capabilities{};
        PlatformQueryMessage message{};

        /// Converts the `SurfaceHdrCapabilityQuery` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;
    };

    struct DisplayQuery {
        vector<DisplayInfo> displays{};
        PlatformQueryMessage message{};

        /// Converts the `DisplayQuery` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;
    };

    /// Converts the value to graphics platform representation.
    ///
    /// @param system `system` value used by the operation.
    ///
    /// @return Returns the value converted to graphics platform representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] GraphicsPlatform::WindowSystem to_graphics_platform(WindowSystem system) noexcept;

    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param transfer `transfer` value used by the operation.
    ///
    /// @return Returns the value converted to RHI representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] HdrTransferFunction to_rhi(GraphicsPlatform::HdrTransferFunction transfer) noexcept;

    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param gamut `gamut` value used by the operation.
    ///
    /// @return Returns the value converted to RHI representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] HdrColorGamut to_rhi(GraphicsPlatform::HdrColorGamut gamut) noexcept;

    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param source Source value or resource.
    ///
    /// @return Returns the value converted to RHI representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] HdrMetadataSource to_rhi(GraphicsPlatform::HdrMetadataSource source) noexcept;

    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param confidence `confidence` value used by the operation.
    ///
    /// @return Returns the value converted to RHI representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] HdrMetadataConfidence to_rhi(GraphicsPlatform::HdrMetadataConfidence confidence) noexcept;

    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param status `status` value used by the operation.
    ///
    /// @return Returns the value converted to RHI representation.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::Ok`, `PlatformQueryStatus::Ok`, `QueryStatus::Unsupported`, `PlatformQueryStatus::Unsupported`, `QueryStatus::NotAvailable`, `PlatformQueryStatus::NotAvailable` among others.
    /// @note This function does not throw exceptions.
    [[nodiscard]] PlatformQueryStatus to_rhi(GraphicsPlatform::QueryStatus status) noexcept;

    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param capabilities `capabilities` value used by the operation.
    ///
    /// @return Returns the value converted to RHI representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] SurfaceHdrCapabilities to_rhi(const GraphicsPlatform::HdrDisplayCapabilities &capabilities);

    /// Converts the backend-specific value to the corresponding RHI representation.
    ///
    /// @param display `display` value used by the operation.
    ///
    /// @return Returns the value converted to RHI representation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] DisplayInfo to_rhi(const GraphicsPlatform::DisplayInfo &display);

    /// Queries platform displays from the active backend or runtime state.
    ///
    /// @return Returns the current query platform displays value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] DisplayQuery query_platform_displays();

    /// Queries platform HDR display capabilities from the active backend or runtime state.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] SurfaceHdrCapabilityQuery query_platform_hdr_display_capabilities(const SurfaceDesc &surface);

} // namespace SFT::RHI
