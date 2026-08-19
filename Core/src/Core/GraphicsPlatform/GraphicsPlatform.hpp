#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SFT::Core::GraphicsPlatform {

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

        /// Converts the `QueryMessage` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept;
    };

    template <typename T>
    struct QueryResult {
        T value{};
        QueryMessage message{};

        /// Converts the `QueryResult` to `bool`.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(message); }
    };

    /// Queries displays from the active backend or runtime state.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::NotAvailable`.
    [[nodiscard]] QueryResult<std::vector<DisplayInfo>> query_displays();
    /// Queries HDR display capabilities from the active backend or runtime state.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] QueryResult<HdrDisplayCapabilities> query_hdr_display_capabilities(const NativeSurfaceHandle &surface);
    /// Compiles the supplied source or pipeline state.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    [[nodiscard]] std::span<const char *const> compiled_backend_notes() noexcept;

} // namespace SFT::Core::GraphicsPlatform
