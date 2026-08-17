#include <Foundation/src/Foundation.hpp>
#include <graphicsPlatform/src/GraphicsPlatform.hpp>

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
            /// Constructs a `ComPtr` in its default state.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            ComPtr() = default;
            /// Disables this construction form for `ComPtr`.
            ///
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            ComPtr(const ComPtr &) = delete;
            /// Assigns a new value to this `ComPtr`.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This overload is deleted; attempting to call it is a compile-time error.
            ComPtr &operator=(const ComPtr &) = delete;
            /// Constructs a `ComPtr` from another instance.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @note This function does not throw exceptions.
            ComPtr(ComPtr &&other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}
            /// Assigns a new value to this `ComPtr`.
            ///
            /// @param other Other object used by the operation.
            ///
            /// @return Returns `*this` so the operation can be chained.
            /// @note This function does not throw exceptions.
            ComPtr &operator=(ComPtr &&other) noexcept {
                if (this != &other) {
                    reset();
                    ptr_ = std::exchange(other.ptr_, nullptr);
                }
                return *this;
            }
            /// Destroys the `ComPtr` and releases resources owned by it.
            ///
            /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
            ~ComPtr() { reset(); }

            /// Returns the value or resource currently represented by `ComPtr`.
            ///
            /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
            /// @note This function does not throw exceptions.
            [[nodiscard]] T *get() const noexcept { return ptr_; }
            /// Returns the current or globally available put value.
            ///
            /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
            /// @note This function does not throw exceptions.
            [[nodiscard]] T **put() noexcept {
                reset();
                return &ptr_;
            }
            /// Accesses the object referenced by this `ComPtr`.
            ///
            /// @return Returns a pointer through which the referenced object can be accessed.
            /// @note This function does not throw exceptions.
            [[nodiscard]] T *operator->() const noexcept { return ptr_; }
            /// Converts the `ComPtr` to `bool`.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] explicit operator bool() const noexcept { return ptr_ != nullptr; }

          private:
            /// Resets the object to its baseline state.
            ///
            /// @note This function does not throw exceptions.
            void reset() noexcept {
                if (ptr_ != nullptr) {
                    ptr_->Release();
                    ptr_ = nullptr;
                }
            }

            T *ptr_ = nullptr;
        };

        /// Performs the narrow wide operation for `GraphicsPlatform` using the supplied arguments.
        ///
        /// @param text Text consumed by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Performs the transfer from DXGI operation for `GraphicsPlatform` using the supplied arguments.
        ///
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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

        /// Performs the gamut from DXGI operation for `GraphicsPlatform` using the supplied arguments.
        ///
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
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

        /// Reports whether HDR color space holds for this `GraphicsPlatform`.
        ///
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_hdr_color_space(DXGI_COLOR_SPACE_TYPE color_space) noexcept {
            return transfer_from_dxgi(color_space) != HdrTransferFunction::Sdr;
        }

        /// Performs the HDR from desc operation for `GraphicsPlatform` using the supplied arguments.
        ///
        /// @param desc Description of the resource or operation to perform.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Enumerates DXGI displays using the supplied arguments and current state.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::PlatformError`, `QueryStatus::Ok`.
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

    /// Queries displays from the active backend or runtime state.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    QueryResult<std::vector<DisplayInfo>> query_displays() {
        return enumerate_dxgi_displays();
    }

    /// Queries HDR display capabilities from the active backend or runtime state.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `QueryStatus::Ok`, `QueryStatus::NotAvailable`, `QueryStatus::Unsupported`.
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
