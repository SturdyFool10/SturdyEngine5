#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::GraphicsPlatform {

    enum class WindowSystem : std::uint32_t {
        Unknown,
        Win32,
        Xlib,
        Xcb,
        Wayland,
        Cocoa,
        Android,
        UIKit,
        WebCanvas,
    };

    struct NativeSurfaceHandle {
        WindowSystem system = WindowSystem::Unknown;
        void *display = nullptr;
        void *window = nullptr;
        std::string_view label{};
    };

    enum class HdrTransferFunction : std::uint32_t {
        Unknown,
        Sdr,
        PqSt2084,
        Hlg,
        LinearExtended,
    };

    enum class HdrColorGamut : std::uint32_t {
        Unknown,
        Rec709,
        DisplayP3,
        Rec2020,
    };

    enum class HdrMetadataSource : std::uint32_t {
        Unknown,
        GraphicsApi,
        OperatingSystem,
        WindowSystem,
        Edid,
        UserCalibration,
        EngineDefault,
    };

    enum class HdrMetadataConfidence : std::uint32_t {
        Unknown,
        Estimated,
        Reported,
        Calibrated,
        Measured,
    };

    struct Chromaticity {
        float x = 0.0f;
        float y = 0.0f;
    };

    struct HdrDisplayMetadata {
        Chromaticity red_primary{};
        Chromaticity green_primary{};
        Chromaticity blue_primary{};
        Chromaticity white_point{};

        float min_luminance_nits = 0.0f;
        float max_luminance_nits = 0.0f;
        float max_full_frame_luminance_nits = 0.0f;

        HdrMetadataSource source = HdrMetadataSource::Unknown;
        HdrMetadataConfidence confidence = HdrMetadataConfidence::Unknown;
    };

    struct HdrPresentationMode {
        HdrTransferFunction transfer = HdrTransferFunction::Unknown;
        HdrColorGamut gamut = HdrColorGamut::Unknown;
        bool requires_os_hdr_mode = true;
    };

    struct HdrDisplayCapabilities {
        bool hdr_supported = false;
        bool hdr_enabled_by_os = false;
        bool hdr_metadata_output_supported = false;

        std::vector<HdrPresentationMode> supported_modes{};
        std::optional<HdrDisplayMetadata> display_metadata{};

        // EDR-style platforms expose headroom rather than HDR10 static metadata. Keep this in the
        // shared shape so Metal/macOS can report useful data without forcing it into HDR10 fields.
        float sdr_white_nits = 80.0f;
        float edr_headroom = 1.0f;
        float max_edr_headroom = 1.0f;
    };

    struct DisplayInfo {
        std::string stable_id{};
        std::string name{};
        bool connected = false;
        bool primary = false;
        HdrDisplayCapabilities hdr{};
    };

    enum class QueryStatus : std::uint32_t {
        Ok,
        Unsupported,
        NotAvailable,
        InvalidArgument,
        PlatformError,
    };

    struct QueryMessage {
        QueryStatus status = QueryStatus::Ok;
        std::string message{};

        [[nodiscard]] explicit operator bool() const noexcept { return status == QueryStatus::Ok; }
    };

    template <typename T>
    struct QueryResult {
        T value{};
        QueryMessage message{};

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(message); }
    };

    [[nodiscard]] QueryResult<std::vector<DisplayInfo>> query_displays();
    [[nodiscard]] QueryResult<HdrDisplayCapabilities> query_hdr_display_capabilities(const NativeSurfaceHandle &surface);
    [[nodiscard]] std::span<const char *const> compiled_backend_notes() noexcept;

} // namespace SFT::GraphicsPlatform
