

#include <D3D12/D3D12Device.hpp>

#pragma region Imports
#include <D3D12/D3D12Convert.hpp>

#include <dcomp.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#pragma endregion

#include <tracy/Tracy.hpp>

namespace SFT::D3D12 {

    namespace {


        constexpr u32 minimum_flip_buffer_count = 2;
        constexpr u32 default_flip_buffer_count = 3;


        /// Converts the value to flip model format representation.
        ///
        /// @param format Format used for the resource, render target, or conversion.
        ///
        /// @return Returns the value converted to flip model format representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DXGI_FORMAT to_flip_model_format(rhi::Format format) noexcept {
            switch (format) {
                case rhi::Format::RGBA8Unorm:
                case rhi::Format::RGBA8UnormSrgb:
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                case rhi::Format::BGRA8Unorm:
                case rhi::Format::BGRA8UnormSrgb:
                    return DXGI_FORMAT_B8G8R8A8_UNORM;
                case rhi::Format::RGB10A2Unorm:
                    return DXGI_FORMAT_R10G10B10A2_UNORM;
                case rhi::Format::RGBA16Float:
                    return DXGI_FORMAT_R16G16B16A16_FLOAT;
                default:
                    return DXGI_FORMAT_UNKNOWN;
            }
        }

        /// Converts the value to DXGI alpha mode representation.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value converted to DXGI alpha mode representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DXGI_ALPHA_MODE to_dxgi_alpha_mode(rhi::CompositeAlphaMode mode) noexcept {
            switch (mode) {
                case rhi::CompositeAlphaMode::Premultiplied:
                    return DXGI_ALPHA_MODE_PREMULTIPLIED;
                case rhi::CompositeAlphaMode::PostMultiplied:
                    return DXGI_ALPHA_MODE_STRAIGHT;
                case rhi::CompositeAlphaMode::Auto:
                case rhi::CompositeAlphaMode::Opaque:
                case rhi::CompositeAlphaMode::Inherit:
                    break;
            }
            return DXGI_ALPHA_MODE_IGNORE;
        }


        struct ResolvedPresent {
            rhi::PresentMode mode = rhi::PresentMode::Fifo;
            u32 sync_interval = 1;
            bool wants_tearing = false;
            bool degraded = false;
            u32 buffer_count = default_flip_buffer_count;
        };

        /// Resolves present into the concrete value used by the engine.
        ///
        /// @param strategy `strategy` value used by the operation.
        /// @param tearing_available `tearing_available` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] ResolvedPresent resolve_present(rhi::PresentStrategy strategy, bool tearing_available) noexcept {
            ResolvedPresent resolved{};
            switch (strategy) {
                case rhi::PresentStrategy::Unsynchronized:
                    if (tearing_available) {


                        resolved = {rhi::PresentMode::Immediate, 0, true, false, default_flip_buffer_count};
                    } else {
                        resolved = {rhi::PresentMode::Mailbox, 0, false, true, default_flip_buffer_count};
                    }
                    break;
                case rhi::PresentStrategy::TearFreeOrdered:
                case rhi::PresentStrategy::VariableRefresh:


                    resolved = {rhi::PresentMode::Fifo, 1, false, false, default_flip_buffer_count};
                    break;
                case rhi::PresentStrategy::TearFreeLatest:
                case rhi::PresentStrategy::TearFreeLatestReady:


                    resolved = {rhi::PresentMode::Mailbox, 0, false, false, default_flip_buffer_count};
                    break;
                case rhi::PresentStrategy::AdaptiveTearing:


                    resolved = {rhi::PresentMode::Fifo, 1, false, true, default_flip_buffer_count};
                    break;
            }
            return resolved;
        }

        /// Returns the current or globally available default hdr10 display metadata value.
        ///
        /// @return Returns the current default hdr10 display metadata value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr rhi::HdrDisplayMetadata default_hdr10_display_metadata() noexcept {
            return rhi::HdrDisplayMetadata{
                .red_primary = {.x = 0.680f, .y = 0.320f},
                .green_primary = {.x = 0.265f, .y = 0.690f},
                .blue_primary = {.x = 0.150f, .y = 0.060f},
                .white_point = {.x = 0.3127f, .y = 0.3290f},
                .min_luminance_nits = 0.001f,
                .max_luminance_nits = 1000.0f,
                .max_full_frame_luminance_nits = 400.0f,
                .source = rhi::HdrMetadataSource::EngineDefault,
                .confidence = rhi::HdrMetadataConfidence::Estimated,
            };
        }

        /// Reports whether valid chromaticity is valid for the current operation.
        ///
        /// @param value Value consumed by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool valid_chromaticity(const rhi::Chromaticity &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && value.x > 0.0f && value.x <= 1.0f &&
                   value.y > 0.0f && value.y <= 1.0f;
        }

        /// Encodes HDR metadata value into the target representation.
        ///
        /// @param value Value consumed by the operation.
        /// @param scale `scale` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        template <typename Integer>
        [[nodiscard]] Integer encode_hdr_metadata_value(f32 value, f64 scale) noexcept {
            if (!std::isfinite(value) || value <= 0.0f) {
                return 0;
            }
            const f64 encoded = static_cast<f64>(value) * scale;
            const f64 maximum = static_cast<f64>((std::numeric_limits<Integer>::max)());
            return static_cast<Integer>(std::min(encoded + 0.5, maximum));
        }

        /// Builds hdr10 metadata.
        ///
        /// @param hdr_query `hdr_query` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] DXGI_HDR_METADATA_HDR10 build_hdr10_metadata(
            const rhi::SurfaceHdrCapabilityQuery &hdr_query) noexcept {
            rhi::HdrDisplayMetadata display = default_hdr10_display_metadata();
            if (hdr_query && hdr_query.capabilities.display_metadata.has_value()) {
                const rhi::HdrDisplayMetadata &reported = *hdr_query.capabilities.display_metadata;
                if (valid_chromaticity(reported.red_primary)) {
                    display.red_primary = reported.red_primary;
                }
                if (valid_chromaticity(reported.green_primary)) {
                    display.green_primary = reported.green_primary;
                }
                if (valid_chromaticity(reported.blue_primary)) {
                    display.blue_primary = reported.blue_primary;
                }
                if (valid_chromaticity(reported.white_point)) {
                    display.white_point = reported.white_point;
                }
                if (std::isfinite(reported.min_luminance_nits) && reported.min_luminance_nits >= 0.0f) {
                    display.min_luminance_nits = reported.min_luminance_nits;
                }
                if (std::isfinite(reported.max_luminance_nits) && reported.max_luminance_nits > 0.0f) {
                    display.max_luminance_nits = reported.max_luminance_nits;
                }
                if (std::isfinite(reported.max_full_frame_luminance_nits) &&
                    reported.max_full_frame_luminance_nits > 0.0f) {
                    display.max_full_frame_luminance_nits = reported.max_full_frame_luminance_nits;
                }
            }

            DXGI_HDR_METADATA_HDR10 metadata{};
            metadata.RedPrimary[0] = encode_hdr_metadata_value<UINT16>(display.red_primary.x, 50000.0);
            metadata.RedPrimary[1] = encode_hdr_metadata_value<UINT16>(display.red_primary.y, 50000.0);
            metadata.GreenPrimary[0] = encode_hdr_metadata_value<UINT16>(display.green_primary.x, 50000.0);
            metadata.GreenPrimary[1] = encode_hdr_metadata_value<UINT16>(display.green_primary.y, 50000.0);
            metadata.BluePrimary[0] = encode_hdr_metadata_value<UINT16>(display.blue_primary.x, 50000.0);
            metadata.BluePrimary[1] = encode_hdr_metadata_value<UINT16>(display.blue_primary.y, 50000.0);
            metadata.WhitePoint[0] = encode_hdr_metadata_value<UINT16>(display.white_point.x, 50000.0);
            metadata.WhitePoint[1] = encode_hdr_metadata_value<UINT16>(display.white_point.y, 50000.0);
            metadata.MaxMasteringLuminance = encode_hdr_metadata_value<UINT>(display.max_luminance_nits, 1.0);
            metadata.MinMasteringLuminance = encode_hdr_metadata_value<UINT>(display.min_luminance_nits, 10000.0);
            metadata.MaxContentLightLevel = encode_hdr_metadata_value<UINT16>(display.max_luminance_nits, 1.0);
            metadata.MaxFrameAverageLightLevel =
                encode_hdr_metadata_value<UINT16>(display.max_full_frame_luminance_nits, 1.0);
            return metadata;
        }

    } // namespace


    /// Creates a surface from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SurfaceHandle> D3D12Device::create_surface(const rhi::SurfaceDesc &desc) {
        if (desc.system != rhi::WindowSystem::Win32) {
            return unsupported("create_surface: the D3D12 backend only presents to Win32 windows.");
        }
        if (desc.window == nullptr) {
            return invalid_argument("create_surface: SurfaceDesc::window must be a valid HWND.");
        }

        SurfaceRecord record{};
        record.hwnd = static_cast<HWND>(desc.window);
        record.desc = desc;
        if (desc.label != nullptr) {
            record.stored_label = desc.label;
        }


        record.desc.label = record.stored_label.empty() ? nullptr : record.stored_label.c_str();
        return surfaces_.insert(std::move(record));
    }

    /// Destroys the surface identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_surface(rhi::SurfaceHandle handle) noexcept { surfaces_.erase(handle); }

    /// Queries HDR capabilities from the active backend or runtime state.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> D3D12Device::query_hdr_capabilities(
        rhi::SurfaceHandle handle) const {
        const SurfaceRecord *record = surfaces_.find(handle);
        if (record == nullptr) {
            return invalid_argument("query_hdr_capabilities: unknown surface handle.");
        }


        return rhi::query_platform_hdr_display_capabilities(record->desc);
    }


    /// Creates a swapchain from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SwapchainHandle> D3D12Device::create_swapchain(const rhi::SwapchainDesc &desc) {
        ZoneScopedN("D3D12Device::create_swapchain");
        const SurfaceRecord *surface = surfaces_.find(desc.surface);
        if (surface == nullptr) {
            return invalid_argument("create_swapchain: unknown surface handle.");
        }
        if (desc.width == 0 || desc.height == 0) {
            return invalid_argument("create_swapchain: width and height must be non-zero.");
        }
        const DXGI_FORMAT back_buffer_format = to_flip_model_format(desc.format);
        if (back_buffer_format == DXGI_FORMAT_UNKNOWN) {
            return unsupported("create_swapchain: the requested format cannot back a DXGI flip-model swapchain.");
        }

        const bool wants_transparency = desc.composite_alpha != rhi::CompositeAlphaMode::Auto &&
                                        desc.composite_alpha != rhi::CompositeAlphaMode::Opaque;
        ResolvedPresent resolved = resolve_present(desc.present_strategy, allow_tearing_);
        if (desc.image_count != 0) {
            resolved.buffer_count = std::clamp(desc.image_count, static_cast<u32>(minimum_flip_buffer_count), static_cast<u32>(DXGI_MAX_SWAP_CHAIN_BUFFERS));
        }


        if (desc.old_swapchain.is_valid()) {
            const SwapchainRecord *old_record = swapchains_.find(desc.old_swapchain);
            if (old_record == nullptr) {
                return invalid_argument("create_swapchain: old_swapchain is not a live swapchain handle.");
            }
            if (old_record->surface != desc.surface) {
                return invalid_argument("create_swapchain: old_swapchain belongs to a different surface.");
            }
            destroy_swapchain(desc.old_swapchain);
        }

        SwapchainRecord record{};
        record.surface = desc.surface;

        DXGI_SWAP_CHAIN_DESC1 swapchain_desc{
            .Width = desc.width,
            .Height = desc.height,
            .Format = back_buffer_format,
            .Stereo = FALSE,
            .SampleDesc = {.Count = 1, .Quality = 0},


            .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
            .BufferCount = resolved.buffer_count,
            .Scaling = DXGI_SCALING_STRETCH,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
            .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
        };
        if (rhi::has_any(desc.usage, rhi::TextureUsage::Sampled)) {
            swapchain_desc.BufferUsage |= DXGI_USAGE_SHADER_INPUT;
        }
        if (resolved.wants_tearing) {
            swapchain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }

        // Always DirectComposition, never CreateSwapChainForHwnd — matching the Vulkan backend
        // (VulkanRhiBridgeSwapchain.cpp attempts composition present for every swapchain generation
        // on a supporting platform, not only transparent ones; see WS_EX_NOREDIRECTIONBITMAP's own
        // doc comment in SDL3Impl.cpp/GlfwWindowNative.cpp, which already documents this as the
        // expected behavior for "every swapchain generation" on both backends).
        //
        // This used to branch on wants_transparency: CreateSwapChainForHwnd for opaque, only
        // switching to CreateSwapChainForComposition (+ a fresh IDCompositionDevice/Target/Visual)
        // when transparency was requested. That branch is exactly what caused the reported ghosting
        // bug — toggling PresentationSettings::transparent_composition rebuilds the swapchain, which
        // transitioned the window off a direct HWND-bound flip-model swapchain and onto a brand-new
        // composition visual tree. Windows' flip model keeps showing an HWND-bound swapchain's last
        // presented frame for that HWND independently of the new composition visual taking over
        // (this is also why a crashed flip-model app leaves a frozen window behind), so the old
        // swapchain's content stayed visible underneath the new transparent one no matter how
        // promptly destroy_swapchain() released it. Never making that transition (always
        // composition, opaque or not) is what the RHI's abstraction already assumes — see
        // RHI::PresentationResolution::via_composition_present's own doc comment — this backend just
        // wasn't honoring it unconditionally.
        const rhi::CompositeAlphaMode effective_alpha =
            wants_transparency
                ? (desc.composite_alpha == rhi::CompositeAlphaMode::Inherit ? rhi::CompositeAlphaMode::Premultiplied
                                                                            : desc.composite_alpha)
                : rhi::CompositeAlphaMode::Opaque;
        swapchain_desc.AlphaMode = to_dxgi_alpha_mode(effective_alpha);
        swapchain_desc.Scaling = DXGI_SCALING_STRETCH;

        ComPtr<IDXGISwapChain1> swapchain1;
        if (const HRESULT hr = factory_->CreateSwapChainForComposition(graphics_queue_.Get(), &swapchain_desc, nullptr, &swapchain1);
            FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (CreateSwapChainForComposition)");
        }
        if (const HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&record.composition_device));
            FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (DCompositionCreateDevice)");
        }
        if (const HRESULT hr = record.composition_device->CreateTargetForHwnd(surface->hwnd, TRUE, &record.composition_target);
            FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (CreateTargetForHwnd)");
        }
        if (const HRESULT hr = record.composition_device->CreateVisual(&record.composition_visual); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (CreateVisual)");
        }
        if (const HRESULT hr = record.composition_visual->SetContent(swapchain1.Get()); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (SetContent)");
        }
        if (const HRESULT hr = record.composition_target->SetRoot(record.composition_visual.Get()); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (SetRoot)");
        }
        if (const HRESULT hr = record.composition_device->Commit(); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (Commit)");
        }

        if (const HRESULT hr = swapchain1.As(&record.swapchain); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (IDXGISwapChain4)");
        }

        record.image_count = resolved.buffer_count;
        record.sync_interval = resolved.sync_interval;
        record.present_flags = resolved.wants_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;


        const u32 frames_in_flight_count =
            desc.frames_in_flight != 0 ? desc.frames_in_flight : std::max<u32>(1, record.image_count);
        if (const HRESULT hr = record.swapchain->SetMaximumFrameLatency(frames_in_flight_count); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (SetMaximumFrameLatency)");
        }


        record.effective_color_space = rhi::ColorSpace::SrgbNonlinear;
        DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        bool color_space_degraded = false;
        if (to_dxgi_color_space(desc.color_space, color_space)) {
            UINT support = 0;
            if (SUCCEEDED(record.swapchain->CheckColorSpaceSupport(color_space, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0 &&
                SUCCEEDED(record.swapchain->SetColorSpace1(color_space))) {
                record.effective_color_space = desc.color_space;
            } else if (desc.color_space != rhi::ColorSpace::SrgbNonlinear) {
                color_space_degraded = true;
                Foundation::log_warn(
                    "D3D12: the requested swapchain color space is not supported by this display; "
                    "presenting as standard sRGB instead.");
            }
        } else if (desc.color_space != rhi::ColorSpace::SrgbNonlinear) {
            color_space_degraded = true;
        }

        if (record.effective_color_space == rhi::ColorSpace::Hdr10St2084) {
            const rhi::SurfaceHdrCapabilityQuery hdr_query =
                rhi::query_platform_hdr_display_capabilities(surface->desc);
            if (!hdr_query || !hdr_query.capabilities.display_metadata.has_value()) {
                Foundation::log_info(
                    "HDR metadata: no real display metadata available for this surface ({}); using conservative "
                    "default primaries/luminance instead.",
                    hdr_query.message.message.empty() ? "platform did not report any" : hdr_query.message.message);
            }
            DXGI_HDR_METADATA_HDR10 metadata = build_hdr10_metadata(hdr_query);
            if (const HRESULT hr = record.swapchain->SetHDRMetaData(
                    DXGI_HDR_METADATA_TYPE_HDR10, static_cast<UINT>(sizeof(metadata)), &metadata);
                FAILED(hr)) {
                return hresult_error(hr, "create_swapchain (SetHDRMetaData)");
            }
            record.stored_hdr_metadata = metadata;
            record.has_hdr_metadata = true;
        }

        record.presentation_resolution = rhi::PresentationResolution{
            .strategy = desc.present_strategy,
            .effective_mode = resolved.mode,
            .degraded = resolved.degraded || color_space_degraded,


            .present_queue_is_compute = false,
            .effective_composite_alpha = effective_alpha,
            .composite_alpha_degraded = wants_transparency && effective_alpha == rhi::CompositeAlphaMode::Opaque,
            .via_composition_present = record.is_composition_present(),


            .supports_completion_fence = false,


            .full_screen_exclusive_active = false,
        };
        (void)desc.allow_present_from_compute;
        (void)desc.request_full_screen_exclusive;
        (void)desc.clipped;

        if (auto created = create_swapchain_textures(record, desc.format, desc.width, desc.height, desc.usage);
            !created) {
            destroy_swapchain_textures(record);
            return std::unexpected(created.error());
        }
        record.frame_latency_waitable = record.swapchain->GetFrameLatencyWaitableObject();
        if (record.frame_latency_waitable == nullptr) {
            destroy_swapchain_textures(record);
            return operation_failed("create_swapchain: DXGI did not provide the requested frame-latency waitable object.");
        }
        set_debug_name(record.swapchain.Get(), desc.label);
        return swapchains_.insert(std::move(record));
    }

    /// Creates a swapchain textures from the supplied parameters.
    ///
    /// @param record `record` value used by the operation.
    /// @param format Format used for the resource, render target, or conversion.
    /// @param width Width of the target extent.
    /// @param height Height of the target extent.
    /// @param usage Usage flags or category applied to the resource.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult D3D12Device::create_swapchain_textures(SwapchainRecord &record, rhi::Format format, u32 width, u32 height, rhi::TextureUsage usage) {
        for (u32 index = 0; index < record.image_count; ++index) {
            ComPtr<ID3D12Resource> buffer;
            if (const HRESULT hr = record.swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer)); FAILED(hr)) {
                return hresult_error(hr, "create_swapchain (GetBuffer)");
            }

            TextureRecord texture{};
            texture.resource = std::move(buffer);


            texture.format = format;
            texture.resource_format = to_flip_model_format(format);
            texture.dimension = rhi::TextureDimension::Dim2D;
            texture.extent = rhi::Extent3D{width, height, 1};
            texture.mip_levels = 1;
            texture.array_layers = 1;
            texture.samples = rhi::SampleCount::X1;
            texture.usage = usage | rhi::TextureUsage::ColorAttachment;
            texture.is_swapchain_image = true;
            if (!enhanced_barriers_) {


                texture.legacy_states.assign(1, D3D12_RESOURCE_STATE_PRESENT);
            }
            set_debug_name(texture.resource.Get(), "Sturdy swapchain back buffer");

            const rhi::TextureHandle texture_handle = textures_.insert(std::move(texture));
            record.textures.push_back(texture_handle);

            auto view = create_texture_view(rhi::TextureViewDesc{
                .texture = texture_handle,
                .view_type = rhi::TextureViewType::View2D,
                .format = format,
                .label = "Sturdy swapchain back buffer view",
            });
            if (!view) {
                return std::unexpected(view.error());
            }
            record.views.push_back(*view);
        }
        return {};
    }

    /// Destroys the swapchain textures identified by the supplied parameters.
    ///
    /// @param record `record` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_swapchain_textures(SwapchainRecord &record) noexcept {
        for (rhi::TextureViewHandle view : record.views) {
            destroy_texture_view(view);
        }
        for (rhi::TextureHandle texture : record.textures) {
            destroy_texture(texture);
        }
        record.views.clear();
        record.textures.clear();
    }

    /// Destroys the swapchain identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Device::destroy_swapchain(rhi::SwapchainHandle handle) noexcept {
        ZoneScopedN("D3D12Device::destroy_swapchain");
        auto record = swapchains_.extract(handle);
        if (!record) {
            return;
        }


        if (record->composition_target != nullptr) {
            (void)record->composition_target->SetRoot(nullptr);
        }
        if (record->composition_device != nullptr) {
            (void)record->composition_device->Commit();
        }


        wait_idle();
        destroy_swapchain_textures(*record);
        if (record->frame_latency_waitable != nullptr) {
            CloseHandle(record->frame_latency_waitable);
        }
    }

    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    rhi::PresentationResolution D3D12Device::presentation_resolution(rhi::SwapchainHandle handle) const noexcept {
        const SwapchainRecord *record = swapchains_.find(handle);
        return record != nullptr ? record->presentation_resolution : rhi::PresentationResolution{};
    }

    /// Updates HDR content light level from the supplied values.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param update `update` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiResult D3D12Device::update_hdr_content_light_level(rhi::SwapchainHandle handle,
                                                               const rhi::HdrContentLightLevelUpdate &update) {
        SwapchainRecord *record = swapchains_.find(handle);
        if (record == nullptr) {
            return invalid_argument("update_hdr_content_light_level: unknown swapchain handle.");
        }
        if (record->effective_color_space != rhi::ColorSpace::Hdr10St2084 || !record->has_hdr_metadata) {
            return unsupported(
                "update_hdr_content_light_level: only an HDR10 ST2084 swapchain has HDR10 metadata to update.");
        }


        DXGI_HDR_METADATA_HDR10 metadata = record->stored_hdr_metadata;
        metadata.MaxContentLightLevel =
            encode_hdr_metadata_value<UINT16>(update.max_content_light_level_nits, 1.0);
        metadata.MaxFrameAverageLightLevel =
            encode_hdr_metadata_value<UINT16>(update.max_frame_average_light_level_nits, 1.0);

        if (const HRESULT hr = record->swapchain->SetHDRMetaData(
                DXGI_HDR_METADATA_TYPE_HDR10, static_cast<UINT>(sizeof(metadata)), &metadata);
            FAILED(hr)) {
            return hresult_error(hr, "update_hdr_content_light_level (SetHDRMetaData)");
        }
        record->stored_hdr_metadata = metadata;
        return {};
    }


    /// Acquires next texture.
    ///
    /// @param swapchain Swapchain used or affected by the operation.
    /// @param frame_slot_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::NotReady`.
    rhi::RhiExpected<rhi::SurfaceTexture> D3D12Device::acquire_next_texture(rhi::SwapchainHandle swapchain,
                                                                            u32 frame_slot_index) {
        ZoneScopedN("D3D12Device::acquire_next_texture");
        SwapchainRecord *record = swapchains_.find(swapchain);
        if (record == nullptr) {
            return invalid_argument("acquire_next_texture: unknown swapchain handle.");
        }


        (void)frame_slot_index;

        if (record->frame_latency_waitable != nullptr) {


            if (WaitForSingleObjectEx(record->frame_latency_waitable, 1000, TRUE) == WAIT_TIMEOUT) {
                return rhi::rhi_error(rhi::RhiErrorCode::NotReady,
                                      "acquire_next_texture: the presentation engine did not release a back buffer.");
            }
        }

        const u32 index = record->swapchain->GetCurrentBackBufferIndex();
        if (index >= record->textures.size()) {
            return operation_failed("acquire_next_texture: DXGI reported a back-buffer index outside the swapchain.");
        }
        record->current_image = index;

        return rhi::SurfaceTexture{
            .swapchain = swapchain,
            .texture = record->textures[index],
            .view = record->views[index],
            .image_index = index,
            .suboptimal = false,


            .composition_present = false,
        };
    }

    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param desc Description of the resource or operation to perform.
    /// @param queue_lock_wait_ms Queue used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::PresentOutcome> D3D12Device::present(const rhi::PresentDesc &desc, f64 *queue_lock_wait_ms) {
        ZoneScopedN("D3D12Device::present");
        SwapchainRecord *record = swapchains_.find(desc.texture.swapchain);
        if (record == nullptr) {
            return invalid_argument("present: unknown swapchain handle.");
        }
        if (desc.completion_fence.is_valid()) {
            return unsupported("present: DXGI exposes no present-completion fence; check "
                               "PresentationResolution::supports_completion_fence before requesting one.");
        }

        const auto lock_started = std::chrono::steady_clock::now();
        auto guard = present_lock_.lock();
        if (queue_lock_wait_ms != nullptr) {
            *queue_lock_wait_ms =
                std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - lock_started).count();
        }

        const DXGI_PRESENT_PARAMETERS parameters{};
        const HRESULT hr = record->swapchain->Present1(record->sync_interval, record->present_flags, &parameters);

        if (hr == DXGI_STATUS_OCCLUDED) {


            return rhi::PresentOutcome::Suboptimal;
        }
        if (hr == DXGI_STATUS_MODE_CHANGED || hr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS) {
            return rhi::PresentOutcome::OutOfDate;
        }
        if (FAILED(hr)) {
            return hresult_error(hr, "present (Present1)");
        }
        if (record->is_composition_present() && record->composition_device != nullptr) {

            (void)record->composition_device->Commit();
        }
        return rhi::PresentOutcome::Success;
    }

} // namespace SFT::D3D12
