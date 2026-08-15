// Surfaces, swapchains, acquisition, and presentation.
//
// ─── Two things DXGI does differently from a Vulkan WSI, both load-bearing ───────────────────────
//
// 1. **The flip model forbids sRGB back-buffer formats.** A flip-model swapchain (the only model
//    worth using, and the only one that can composite) accepts exactly R8G8B8A8_UNORM,
//    B8G8R8A8_UNORM, R10G10B10A2_UNORM, and R16G16B16A16_FLOAT. An sRGB swapchain is expressed by
//    creating the buffers as UNORM and putting the _SRGB format on the *render target view* — which
//    is a documented, explicitly-supported combination, not a trick. So a caller asking for
//    BGRA8UnormSrgb gets a B8G8R8A8_UNORM swapchain whose views are B8G8R8A8_UNORM_SRGB, and sees no
//    difference: the same shader output lands correctly encoded either way.
//
// 2. **There is no acquire operation.** Vulkan's vkAcquireNextImageKHR both picks an image and gives
//    you a semaphore proving it is free; DXGI's GetCurrentBackBufferIndex only picks. What throttles
//    the CPU instead is the *frame-latency waitable object*, which this backend requests on every
//    swapchain and waits on inside acquire_next_texture(). That places the block at the same point in
//    the frame a Vulkan acquire would, so a caller's frame pacing behaves the same on both backends.
//    `frame_slot_index` consequently has nothing to select here — see acquire_next_texture().
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

        // Buffers a flip-model swapchain needs. Two is the minimum DXGI accepts; three is what any
        // tear-free-but-latest strategy needs to have a spare frame queued without stalling.
        constexpr u32 minimum_flip_buffer_count = 2;
        constexpr u32 default_flip_buffer_count = 3;

        // The back-buffer format for `format`, or DXGI_FORMAT_UNKNOWN when the flip model cannot
        // present it at all. See this file's header comment for why sRGB collapses to UNORM.
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

        // How a PresentStrategy is realized on DXGI, plus whether that realization is a genuine match
        // or a fallback the caller must be told about through PresentationResolution::degraded.
        struct ResolvedPresent {
            rhi::PresentMode mode = rhi::PresentMode::Fifo;
            u32 sync_interval = 1;
            bool wants_tearing = false;
            bool degraded = false;
            u32 buffer_count = default_flip_buffer_count;
        };

        [[nodiscard]] ResolvedPresent resolve_present(rhi::PresentStrategy strategy, bool tearing_available) noexcept {
            ResolvedPresent resolved{};
            switch (strategy) {
                case rhi::PresentStrategy::Unsynchronized:
                    if (tearing_available) {
                        // The only real "VSync off" DXGI has: sync interval 0 *plus* the tearing flag.
                        // Sync interval 0 alone does not tear — it replaces the queued frame instead,
                        // which is mailbox behaviour, not immediate.
                        resolved = {rhi::PresentMode::Immediate, 0, true, false, default_flip_buffer_count};
                    } else {
                        resolved = {rhi::PresentMode::Mailbox, 0, false, true, default_flip_buffer_count};
                    }
                    break;
                case rhi::PresentStrategy::TearFreeOrdered:
                case rhi::PresentStrategy::VariableRefresh:
                    // Plain FIFO, exactly as both strategies ask for. VariableRefresh is deliberately
                    // identical: VRR is a display/driver pacing behaviour rather than a present mode,
                    // so requesting it changes nothing about the present call itself.
                    resolved = {rhi::PresentMode::Fifo, 1, false, false, default_flip_buffer_count};
                    break;
                case rhi::PresentStrategy::TearFreeLatest:
                case rhi::PresentStrategy::TearFreeLatestReady:
                    // Sync interval 0 without the tearing flag: the presentation engine discards
                    // older queued frames and shows the newest at vblank, never tearing. That is
                    // Mailbox, and it is a genuine match rather than a fallback.
                    resolved = {rhi::PresentMode::Mailbox, 0, false, false, default_flip_buffer_count};
                    break;
                case rhi::PresentStrategy::AdaptiveTearing:
                    // DXGI has no per-frame "tear only if late" mode — the tearing flag is an
                    // all-or-nothing property of each Present call, with no lateness signal to key it
                    // off. Presenting at sync interval 1 keeps the tear-free guarantee, which is the
                    // safe half of the request, and the caller is told it was degraded.
                    resolved = {rhi::PresentMode::Fifo, 1, false, true, default_flip_buffer_count};
                    break;
            }
            return resolved;
        }

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

        [[nodiscard]] bool valid_chromaticity(const rhi::Chromaticity &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && value.x > 0.0f && value.x <= 1.0f &&
                   value.y > 0.0f && value.y <= 1.0f;
        }

        template <typename Integer>
        [[nodiscard]] Integer encode_hdr_metadata_value(f32 value, f64 scale) noexcept {
            if (!std::isfinite(value) || value <= 0.0f) {
                return 0;
            }
            const f64 encoded = static_cast<f64>(value) * scale;
            const f64 maximum = static_cast<f64>((std::numeric_limits<Integer>::max)());
            return static_cast<Integer>(std::min(encoded + 0.5, maximum));
        }

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

    // ─── Surfaces ────────────────────────────────────────────────────────────────

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
        // The stored copy, never the caller's pointer — query_hdr_capabilities() may run long after
        // the caller's label string has gone away.
        record.desc.label = record.stored_label.empty() ? nullptr : record.stored_label.c_str();
        return surfaces_.insert(std::move(record));
    }

    void D3D12Device::destroy_surface(rhi::SurfaceHandle handle) noexcept { surfaces_.erase(handle); }

    rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> D3D12Device::query_hdr_capabilities(
        rhi::SurfaceHandle handle) const {
        const SurfaceRecord *record = surfaces_.find(handle);
        if (record == nullptr) {
            return invalid_argument("query_hdr_capabilities: unknown surface handle.");
        }
        // Answered by graphicsPlatform's OS-level display query rather than by DXGI's own
        // IDXGIOutput6::GetDesc1. The two overlap, but only the platform query reaches the things a
        // caller actually has to branch on — whether the *OS* has HDR turned on for that display, and
        // the EDID-derived mastering metadata — which DXGI does not report at all.
        return rhi::query_platform_hdr_display_capabilities(record->desc);
    }

    // ─── Swapchains ──────────────────────────────────────────────────────────────

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

        // DXGI flip-model swapchains have a one-per-HWND rule. Vulkan can hand the retiring
        // swapchain to vkCreateSwapchainKHR, but DXGI has no equivalent: the old object must be
        // released before CreateSwapChainForHwnd. The renderer only reaches this point after it has
        // drained the previous present, and destroy_swapchain() additionally waits idle before
        // releasing the back buffers.
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
            // A flip-model back buffer is never multisampled — MSAA is resolved into it, never
            // rendered directly into it, which is why SampleDesc is fixed at 1 above.
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

        rhi::CompositeAlphaMode effective_alpha = rhi::CompositeAlphaMode::Opaque;
        ComPtr<IDXGISwapChain1> swapchain1;

        if (wants_transparency) {
            // The composition path. Unlike the Vulkan backend — which has to allocate its own shared
            // textures and blit into a DXGI back buffer once per frame, because DXGI buffers cannot be
            // imported into Vulkan — D3D12 renders straight into the composition swapchain's own back
            // buffers. Same transparency, none of the per-frame copy.
            swapchain_desc.AlphaMode = to_dxgi_alpha_mode(desc.composite_alpha == rhi::CompositeAlphaMode::Inherit
                                                              ? rhi::CompositeAlphaMode::Premultiplied
                                                              : desc.composite_alpha);
            // A composition swapchain is HWND-less and must be sized in whole pixels with no stretch
            // scaling, or the compositor resamples every frame.
            swapchain_desc.Scaling = DXGI_SCALING_STRETCH;

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
            effective_alpha = desc.composite_alpha == rhi::CompositeAlphaMode::Inherit
                                  ? rhi::CompositeAlphaMode::Premultiplied
                                  : desc.composite_alpha;
        } else {
            if (const HRESULT hr = factory_->CreateSwapChainForHwnd(graphics_queue_.Get(), surface->hwnd, &swapchain_desc, nullptr, nullptr, &swapchain1);
                FAILED(hr)) {
                return hresult_error(hr, "create_swapchain (CreateSwapChainForHwnd)");
            }
            // Alt+Enter's automatic mode switch is DXGI second-guessing the application's own
            // fullscreen handling; the engine owns window mode, so it is disabled here rather than
            // fought later.
            (void)factory_->MakeWindowAssociation(surface->hwnd, DXGI_MWA_NO_ALT_ENTER);
        }

        if (const HRESULT hr = swapchain1.As(&record.swapchain); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (IDXGISwapChain4)");
        }

        record.image_count = resolved.buffer_count;
        record.sync_interval = resolved.sync_interval;
        record.present_flags = resolved.wants_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u;

        // Match DXGI's queued-frame limit to the caller's already-resolved CPU frame-slot count. The
        // documented zero fallback mirrors SwapchainDesc::frames_in_flight and the Vulkan backend.
        const u32 frames_in_flight_count =
            desc.frames_in_flight != 0 ? desc.frames_in_flight : std::max<u32>(1, record.image_count);
        if (const HRESULT hr = record.swapchain->SetMaximumFrameLatency(frames_in_flight_count); FAILED(hr)) {
            return hresult_error(hr, "create_swapchain (SetMaximumFrameLatency)");
        }

        // Color space is negotiated, never assumed: an HDR color space on a display the OS has HDR
        // switched off for is rejected here rather than silently producing wrong colors. Retain what
        // was actually selected so metadata operations cannot key off the request after a fallback.
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
            // DXGI binds a swapchain to the queue it was created against and offers no equivalent of
            // presenting from a different queue family, so this is always false regardless of the
            // request — reported honestly rather than echoed back.
            .present_queue_is_compute = false,
            .effective_composite_alpha = effective_alpha,
            .composite_alpha_degraded = wants_transparency && effective_alpha == rhi::CompositeAlphaMode::Opaque,
            .via_composition_present = record.is_composition_present(),
            // PresentDesc::completion_fence has no DXGI counterpart: there is no per-present
            // completion object to attach a fence to. Reported unsupported per-swapchain, which is
            // exactly what that field documents callers must check.
            .supports_completion_fence = false,
            // Exclusive fullscreen is reachable through SetFullscreenState, but it is a window-mode
            // decision the platform layer owns, and this backend never enters it behind the caller's
            // back — so it is never claimed active.
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

    rhi::RhiResult D3D12Device::create_swapchain_textures(SwapchainRecord &record, rhi::Format format, u32 width, u32 height, rhi::TextureUsage usage) {
        for (u32 index = 0; index < record.image_count; ++index) {
            ComPtr<ID3D12Resource> buffer;
            if (const HRESULT hr = record.swapchain->GetBuffer(index, IID_PPV_ARGS(&buffer)); FAILED(hr)) {
                return hresult_error(hr, "create_swapchain (GetBuffer)");
            }

            TextureRecord texture{};
            texture.resource = std::move(buffer);
            // The *requested* RHI format, including its sRGB-ness, even though the resource itself is
            // UNORM — that is what makes create_texture_view() build an _SRGB render target view over
            // it, which is the whole mechanism described in this file's header comment.
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
                // A freshly-presented back buffer is in PRESENT state, which is the state a legacy
                // barrier must name as the "before" state of the first transition each frame.
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

    void D3D12Device::destroy_swapchain(rhi::SwapchainHandle handle) noexcept {
        ZoneScopedN("D3D12Device::destroy_swapchain");
        auto record = swapchains_.extract(handle);
        if (!record) {
            return;
        }
        // A composition swapchain's visual tree outlives the swapchain object in DWM's eyes: simply
        // releasing the ComPtrs below detaches this process's references, but DWM keeps compositing
        // the last frame the visual ever committed until the root is explicitly cleared and that
        // clear is committed. Skipping this (e.g. on a resize, which recreates the whole composition
        // device/target/visual trio via create_swapchain's old_swapchain path) leaves the old,
        // now-destroyed swapchain's last presented frame visible underneath/alongside the new one —
        // read as a frozen/pasted-over frame. Mirrors CompositionPresent.cpp's teardown on the Vulkan
        // side.
        if (record->composition_target != nullptr) {
            (void)record->composition_target->SetRoot(nullptr);
        }
        if (record->composition_device != nullptr) {
            (void)record->composition_device->Commit();
        }
        // Back buffers are referenced by the presentation engine until every queued present has
        // retired, and releasing them earlier is a use-after-free the runtime reports as a device
        // removal. Draining is the only way to know they are free.
        wait_idle();
        destroy_swapchain_textures(*record);
        if (record->frame_latency_waitable != nullptr) {
            CloseHandle(record->frame_latency_waitable);
        }
    }

    rhi::PresentationResolution D3D12Device::presentation_resolution(rhi::SwapchainHandle handle) const noexcept {
        const SwapchainRecord *record = swapchains_.find(handle);
        return record != nullptr ? record->presentation_resolution : rhi::PresentationResolution{};
    }

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

        // Work on a copy so both the mastering-display fields and the last successfully-submitted CLL
        // values remain intact if DXGI rejects the update. Only MaxCLL and MaxFALL are overwritten.
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

    // ─── Acquire / present ───────────────────────────────────────────────────────

    rhi::RhiExpected<rhi::SurfaceTexture> D3D12Device::acquire_next_texture(rhi::SwapchainHandle swapchain,
                                                                            u32 frame_slot_index) {
        ZoneScopedN("D3D12Device::acquire_next_texture");
        SwapchainRecord *record = swapchains_.find(swapchain);
        if (record == nullptr) {
            return invalid_argument("acquire_next_texture: unknown swapchain handle.");
        }

        // `frame_slot_index` selects an acquisition semaphore under Vulkan. DXGI has no such object
        // (see this file's header comment), so there is nothing to index — the waitable object below
        // provides the same throttling for every slot. Accepted and ignored rather than rejected, so
        // one caller works unchanged against both backends.
        (void)frame_slot_index;

        if (record->frame_latency_waitable != nullptr) {
            // A finite wait, not INFINITE: a swapchain whose window was destroyed never signals, and
            // reporting NotReady lets the caller skip a frame and re-check rather than deadlock. The
            // RHI documents NotReady as safe to retry with the same slot index.
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
            // False even on the composition path: SurfaceTexture::composition_present exists to warn a
            // renderer that its images are *external* resources needing a general layout instead of
            // TextureLayout::Present. A D3D12 composition swapchain's buffers are ordinary DXGI back
            // buffers, so the normal Present layout is correct and the caveat does not apply.
            .composition_present = false,
        };
    }

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
            // The window is fully hidden. The present succeeded and the swapchain is still usable, but
            // DXGI throttles further presents until it is visible again — Suboptimal is exactly that
            // "works, but rebuild/re-check soon" meaning.
            return rhi::PresentOutcome::Suboptimal;
        }
        if (hr == DXGI_STATUS_MODE_CHANGED || hr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS) {
            return rhi::PresentOutcome::OutOfDate;
        }
        if (FAILED(hr)) {
            return hresult_error(hr, "present (Present1)");
        }
        if (record->is_composition_present() && record->composition_device != nullptr) {
            // The compositor only picks up the new frame once the visual tree is committed.
            (void)record->composition_device->Commit();
        }
        return rhi::PresentOutcome::Success;
    }

} // namespace SFT::D3D12
