#include <graphicsPlatform/GraphicsPlatform.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace SFT::GraphicsPlatform {

    namespace {

        template <typename T>
        class ComPtr {
          public:
            ComPtr() = default;
            ComPtr(const ComPtr &) = delete;
            ComPtr &operator=(const ComPtr &) = delete;
            ComPtr(ComPtr &&other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
            ComPtr &operator=(ComPtr &&other) noexcept {
                if (this != &other) {
                    reset();
                    ptr_ = std::exchange(other.ptr_, nullptr);
                }
                return *this;
            }
            ~ComPtr() { reset(); }

            [[nodiscard]] T *get() const noexcept { return ptr_; }
            [[nodiscard]] T **put() noexcept {
                reset();
                return &ptr_;
            }
            [[nodiscard]] T *operator->() const noexcept { return ptr_; }
            [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

          private:
            void reset() noexcept {
                if (ptr_ != nullptr) {
                    ptr_->Release();
                    ptr_ = nullptr;
                }
            }

            T *ptr_ = nullptr;
        };

        [[nodiscard]] std::string narrow_wide(const wchar_t *text) {
            if (text == nullptr || text[0] == L'\0') {
                return {};
            }
            const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            if (required <= 1) {
                return {};
            }
            std::string out(static_cast<std::size_t>(required - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), required, nullptr, nullptr);
            return out;
        }

        [[nodiscard]] HdrTransferFunction transfer_from_dxgi(DXGI_COLOR_SPACE_TYPE color_space) noexcept {
            switch (color_space) {
                case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
                    return HdrTransferFunction::PqSt2084;
                case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
                case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
                    return HdrTransferFunction::Hlg;
                case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
                    return HdrTransferFunction::LinearExtended;
                default:
                    return HdrTransferFunction::Sdr;
            }
        }

        [[nodiscard]] HdrColorGamut gamut_from_dxgi(DXGI_COLOR_SPACE_TYPE color_space) noexcept {
            switch (color_space) {
                case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
                case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
                case DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020:
                case DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020:
                    return HdrColorGamut::Rec2020;
                default:
                    return HdrColorGamut::Rec709;
            }
        }

        [[nodiscard]] bool is_hdr_color_space(DXGI_COLOR_SPACE_TYPE color_space) noexcept {
            return transfer_from_dxgi(color_space) != HdrTransferFunction::Sdr;
        }

        [[nodiscard]] HdrDisplayCapabilities hdr_from_desc(const DXGI_OUTPUT_DESC1 &desc) {
            const HdrTransferFunction transfer = transfer_from_dxgi(desc.ColorSpace);
            const HdrColorGamut gamut = gamut_from_dxgi(desc.ColorSpace);
            HdrDisplayCapabilities capabilities{
                .hdr_supported = is_hdr_color_space(desc.ColorSpace) || desc.MaxLuminance > 0.0f,
                .hdr_enabled_by_os = is_hdr_color_space(desc.ColorSpace),
                .hdr_metadata_output_supported = true,
                .sdr_white_nits = 80.0f,
                .edr_headroom = desc.MaxLuminance > 0.0f ? std::max(1.0f, desc.MaxLuminance / 80.0f) : 1.0f,
                .max_edr_headroom = desc.MaxLuminance > 0.0f ? std::max(1.0f, desc.MaxLuminance / 80.0f) : 1.0f,
            };
            if (capabilities.hdr_supported) {
                capabilities.supported_modes.push_back(HdrPresentationMode{
                    .transfer = transfer,
                    .gamut = gamut,
                    .requires_os_hdr_mode = true,
                });
                capabilities.display_metadata = HdrDisplayMetadata{
                    .red_primary = Chromaticity{.x = desc.RedPrimary[0], .y = desc.RedPrimary[1]},
                    .green_primary = Chromaticity{.x = desc.GreenPrimary[0], .y = desc.GreenPrimary[1]},
                    .blue_primary = Chromaticity{.x = desc.BluePrimary[0], .y = desc.BluePrimary[1]},
                    .white_point = Chromaticity{.x = desc.WhitePoint[0], .y = desc.WhitePoint[1]},
                    .min_luminance_nits = desc.MinLuminance,
                    .max_luminance_nits = desc.MaxLuminance,
                    .max_full_frame_luminance_nits = desc.MaxFullFrameLuminance,
                    .source = HdrMetadataSource::OperatingSystem,
                    .confidence = HdrMetadataConfidence::Reported,
                };
            }
            return capabilities;
        }

        [[nodiscard]] QueryResult<std::vector<DisplayInfo>> enumerate_dxgi_displays() {
            ComPtr<IDXGIFactory6> factory;
            HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
            if (FAILED(hr)) {
                return QueryResult<std::vector<DisplayInfo>>{
                    .value = {},
                    .message = QueryMessage{.status = QueryStatus::PlatformError, .message = "CreateDXGIFactory1 failed."},
                };
            }

            std::vector<DisplayInfo> displays;
            for (UINT adapter_index = 0;; ++adapter_index) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(adapter_index, adapter.put()) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                if (!adapter) {
                    continue;
                }

                for (UINT output_index = 0;; ++output_index) {
                    ComPtr<IDXGIOutput> output;
                    if (adapter->EnumOutputs(output_index, output.put()) == DXGI_ERROR_NOT_FOUND) {
                        break;
                    }
                    if (!output) {
                        continue;
                    }

                    ComPtr<IDXGIOutput6> output6;
                    if (FAILED(output->QueryInterface(IID_PPV_ARGS(output6.put()))) || !output6) {
                        continue;
                    }

                    DXGI_OUTPUT_DESC1 desc{};
                    if (FAILED(output6->GetDesc1(&desc))) {
                        continue;
                    }
                    const std::string name = narrow_wide(desc.DeviceName);
                    displays.push_back(DisplayInfo{
                        .stable_id = name,
                        .name = name,
                        .connected = desc.AttachedToDesktop == TRUE,
                        .primary = desc.AttachedToDesktop == TRUE && desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0,
                        .hdr = hdr_from_desc(desc),
                    });
                }
            }

            return QueryResult<std::vector<DisplayInfo>>{
                .value = std::move(displays),
                .message = QueryMessage{.status = QueryStatus::Ok, .message = "Windows DXGI display enumeration completed."},
            };
        }

    } // namespace

    QueryResult<std::vector<DisplayInfo>> query_displays() {
        return enumerate_dxgi_displays();
    }

    QueryResult<HdrDisplayCapabilities> query_hdr_display_capabilities(const NativeSurfaceHandle &surface) {
        auto displays = enumerate_dxgi_displays();
        if (!displays) {
            return QueryResult<HdrDisplayCapabilities>{.value = {}, .message = displays.message};
        }

        (void)surface;

        auto hdr_display = std::ranges::find_if(displays.value, [](const DisplayInfo &display) {
            return display.connected && display.hdr.hdr_supported;
        });
        if (hdr_display != displays.value.end()) {
            return QueryResult<HdrDisplayCapabilities>{
                .value = hdr_display->hdr,
                .message = QueryMessage{.status = QueryStatus::Ok, .message = "Returned best connected DXGI HDR display metadata."},
            };
        }

        return QueryResult<HdrDisplayCapabilities>{
            .value = {},
            .message = QueryMessage{
                .status = displays.value.empty() ? QueryStatus::NotAvailable : QueryStatus::Unsupported,
                .message = displays.value.empty() ? "No DXGI displays were available." : "No connected DXGI display reported HDR metadata.",
            },
        };
    }

} // namespace SFT::GraphicsPlatform
