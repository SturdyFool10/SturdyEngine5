#include <Foundation/src/Foundation.hpp>
#include <graphicsPlatform/src/GraphicsPlatform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SFT::GraphicsPlatform {

    namespace {

        constexpr std::array<std::uint8_t, 8> edid_header{0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};

        [[nodiscard]] std::optional<std::vector<std::uint8_t>> read_binary_file(const std::filesystem::path &path) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> data(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
            if (data.size() < 128) {
                return std::nullopt;
            }
            return data;
        }

        [[nodiscard]] std::string read_text_file(const std::filesystem::path &path) {
            std::ifstream file(path);
            std::string text;
            std::getline(file, text);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) {
                text.pop_back();
            }
            return text;
        }

        [[nodiscard]] bool is_connected(const std::filesystem::path &connector) {
            return read_text_file(connector / "status") == "connected";
        }

        [[nodiscard]] float chromaticity_from_10_bit(std::uint16_t value) noexcept {
            return static_cast<float>(value) / 1024.0f;
        }

        [[nodiscard]] HdrDisplayMetadata parse_base_chromaticity(const std::vector<std::uint8_t> &edid) noexcept {
            const std::uint8_t low_rx = static_cast<std::uint8_t>((edid[25] >> 6) & 0x3);
            const std::uint8_t low_ry = static_cast<std::uint8_t>((edid[25] >> 4) & 0x3);
            const std::uint8_t low_gx = static_cast<std::uint8_t>((edid[25] >> 2) & 0x3);
            const std::uint8_t low_gy = static_cast<std::uint8_t>(edid[25] & 0x3);
            const std::uint8_t low_bx = static_cast<std::uint8_t>((edid[26] >> 6) & 0x3);
            const std::uint8_t low_by = static_cast<std::uint8_t>((edid[26] >> 4) & 0x3);
            const std::uint8_t low_wx = static_cast<std::uint8_t>((edid[26] >> 2) & 0x3);
            const std::uint8_t low_wy = static_cast<std::uint8_t>(edid[26] & 0x3);

            auto combine = [](std::uint8_t high, std::uint8_t low) noexcept -> float {
                return chromaticity_from_10_bit(static_cast<std::uint16_t>((static_cast<std::uint16_t>(high) << 2) | low));
            };

            return HdrDisplayMetadata{
                .red_primary = Chromaticity{.x = combine(edid[27], low_rx), .y = combine(edid[28], low_ry)},
                .green_primary = Chromaticity{.x = combine(edid[29], low_gx), .y = combine(edid[30], low_gy)},
                .blue_primary = Chromaticity{.x = combine(edid[31], low_bx), .y = combine(edid[32], low_by)},
                .white_point = Chromaticity{.x = combine(edid[33], low_wx), .y = combine(edid[34], low_wy)},
                .source = HdrMetadataSource::Edid,
                .confidence = HdrMetadataConfidence::Reported,
            };
        }

        [[nodiscard]] float cta_luminance_code_to_nits(std::uint8_t code) noexcept {
            if (code == 0) {
                return 0.0f;
            }
            return 50.0f * std::pow(2.0f, static_cast<float>(code) / 32.0f);
        }

        [[nodiscard]] float cta_min_luminance_code_to_nits(std::uint8_t code, float max_luminance) noexcept {
            if (code == 0 || max_luminance <= 0.0f) {
                return 0.0f;
            }
            const float normalized = static_cast<float>(code) / 255.0f;
            return max_luminance * normalized * normalized / 100.0f;
        }

        void parse_cta_hdr_static_metadata(const std::vector<std::uint8_t> &edid, HdrDisplayCapabilities &capabilities) {
            const std::size_t extension_count = edid[126];
            for (std::size_t extension_index = 0; extension_index < extension_count; ++extension_index) {
                const std::size_t base = 128u * (extension_index + 1u);
                if (base + 128u > edid.size() || edid[base] != 0x02) {
                    continue;
                }

                const std::uint8_t dtd_offset = edid[base + 2] == 0 ? 127 : edid[base + 2];
                std::size_t offset = base + 4;
                const std::size_t end = std::min<std::size_t>(base + dtd_offset, base + 127);
                while (offset < end) {
                    const std::uint8_t header = edid[offset++];
                    const std::uint8_t tag = static_cast<std::uint8_t>((header >> 5) & 0x7);
                    const std::uint8_t length = static_cast<std::uint8_t>(header & 0x1f);
                    if (length == 0 || offset + length > end) {
                        offset += length;
                        continue;
                    }

                    if (tag == 0x7 && edid[offset] == 0x06 && length >= 3) {
                        const std::uint8_t eotf = edid[offset + 1];
                        if ((eotf & (1u << 2u)) != 0) {
                            capabilities.supported_modes.push_back(HdrPresentationMode{
                                .transfer = HdrTransferFunction::PqSt2084,
                                .gamut = HdrColorGamut::Rec2020,
                                .requires_os_hdr_mode = true,
                            });
                        }
                        if ((eotf & (1u << 3u)) != 0) {
                            capabilities.supported_modes.push_back(HdrPresentationMode{
                                .transfer = HdrTransferFunction::Hlg,
                                .gamut = HdrColorGamut::Rec2020,
                                .requires_os_hdr_mode = true,
                            });
                        }

                        capabilities.hdr_supported = !capabilities.supported_modes.empty();
                        capabilities.hdr_metadata_output_supported = capabilities.hdr_supported;
                        if (!capabilities.display_metadata.has_value()) {
                            capabilities.display_metadata = HdrDisplayMetadata{};
                        }
                        HdrDisplayMetadata &metadata = *capabilities.display_metadata;
                        if (length >= 4) {
                            metadata.max_luminance_nits = cta_luminance_code_to_nits(edid[offset + 3]);
                        }
                        if (length >= 5) {
                            metadata.max_full_frame_luminance_nits = cta_luminance_code_to_nits(edid[offset + 4]);
                        }
                        if (length >= 6) {
                            metadata.min_luminance_nits = cta_min_luminance_code_to_nits(edid[offset + 5], metadata.max_luminance_nits);
                        }
                        metadata.source = HdrMetadataSource::Edid;
                        metadata.confidence = HdrMetadataConfidence::Reported;
                    }
                    offset += length;
                }
            }
        }

        [[nodiscard]] std::optional<HdrDisplayCapabilities> parse_edid_hdr_capabilities(const std::vector<std::uint8_t> &edid) {
            if (edid.size() < 128 || !std::equal(edid_header.begin(), edid_header.end(), edid.begin())) {
                return std::nullopt;
            }

            HdrDisplayCapabilities capabilities{};
            capabilities.display_metadata = parse_base_chromaticity(edid);
            parse_cta_hdr_static_metadata(edid, capabilities);
            if (!capabilities.hdr_supported) {
                capabilities.display_metadata.reset();
            }
            return capabilities;
        }

        [[nodiscard]] DisplayInfo display_from_drm_connector(const std::filesystem::path &connector) {
            DisplayInfo display{};
            display.stable_id = connector.filename().string();
            display.name = display.stable_id;
            display.connected = is_connected(connector);
            if (!display.connected) {
                return display;
            }

            if (auto edid = read_binary_file(connector / "edid"); edid.has_value()) {
                if (auto capabilities = parse_edid_hdr_capabilities(*edid); capabilities.has_value()) {
                    display.hdr = std::move(*capabilities);
                }
            }
            return display;
        }

        [[nodiscard]] std::vector<DisplayInfo> enumerate_drm_displays() {
            std::vector<DisplayInfo> displays;
            const std::filesystem::path drm_root{"/sys/class/drm"};
            std::error_code ec;
            if (!std::filesystem::exists(drm_root, ec)) {
                return displays;
            }

            for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(drm_root, ec)) {
                if (ec || !entry.is_directory(ec)) {
                    continue;
                }
                const std::filesystem::path connector = entry.path();
                if (!std::filesystem::exists(connector / "status", ec)) {
                    continue;
                }
                displays.push_back(display_from_drm_connector(connector));
            }
            std::ranges::sort(displays, {}, &DisplayInfo::stable_id);
            return displays;
        }

        [[nodiscard]] QueryResult<HdrDisplayCapabilities> best_hdr_capabilities_from_displays(std::vector<DisplayInfo> displays,
                                                                                              const char *detail) {
            auto hdr_display = std::ranges::find_if(displays, [](const DisplayInfo &display) {
                return display.connected && display.hdr.hdr_supported;
            });
            if (hdr_display != displays.end()) {
                return QueryResult<HdrDisplayCapabilities>{
                    .value = hdr_display->hdr,
                    .message = QueryMessage{.status = QueryStatus::Ok, .message = detail},
                };
            }

            HdrDisplayCapabilities fallback{};
            return QueryResult<HdrDisplayCapabilities>{
                .value = fallback,
                .message = QueryMessage{
                    .status = displays.empty() ? QueryStatus::NotAvailable : QueryStatus::Unsupported,
                    .message = displays.empty()
                        ? "No DRM connector EDID data was available for HDR metadata querying."
                        : "No connected DRM display reported CTA-861 HDR static metadata in EDID.",
                },
            };
        }

    } // namespace

    QueryResult<std::vector<DisplayInfo>> query_displays() {
        std::vector<DisplayInfo> displays = enumerate_drm_displays();
        return QueryResult<std::vector<DisplayInfo>>{
            .value = std::move(displays),
            .message = QueryMessage{
                .status = QueryStatus::Ok,
                .message = "Linux DRM sysfs display enumeration completed.",
            },
        };
    }

    QueryResult<HdrDisplayCapabilities> query_hdr_display_capabilities(const NativeSurfaceHandle &surface) {
        std::vector<DisplayInfo> displays = enumerate_drm_displays();
        switch (surface.system) {
            case WindowSystem::Wayland:
                return best_hdr_capabilities_from_displays(
                    std::move(displays),
                    "Wayland per-window HDR metadata is compositor/protocol dependent; returned best connected DRM EDID HDR metadata.");
            case WindowSystem::Xlib:
            case WindowSystem::Xcb:
                return best_hdr_capabilities_from_displays(
                    std::move(displays),
                    "X11 per-window monitor matching is not wired yet; returned best connected DRM EDID HDR metadata.");
            case WindowSystem::Unknown:
                return QueryResult<HdrDisplayCapabilities>{
                    .value = HdrDisplayCapabilities{},
                    .message = QueryMessage{
                        .status = QueryStatus::InvalidArgument,
                        .message = "Cannot query HDR display capabilities for an unknown window system.",
                    },
                };
            default:
                return QueryResult<HdrDisplayCapabilities>{
                    .value = HdrDisplayCapabilities{},
                    .message = QueryMessage{
                        .status = QueryStatus::Unsupported,
                        .message = "This Linux graphicsPlatform build cannot query HDR metadata for the provided window system.",
                    },
                };
        }
    }

} // namespace SFT::GraphicsPlatform
