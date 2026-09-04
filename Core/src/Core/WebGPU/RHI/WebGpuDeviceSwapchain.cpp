#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <Core/WebGPU/RHI/WebGpuConvert.hpp>

namespace SFT::Core::WebGpu {

    // WebGPU has no swapchain object. A surface is configured once with a size, format, and present
    // mode, and each frame asks it for the current texture; resizing means reconfiguring the same
    // surface. The RHI's surface/swapchain split is preserved by making a swapchain a thin handle
    // that names the surface it configured, which is what the pool below stores.

    /// Creates a presentation surface for a native window.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SurfaceHandle> WebGpuDevice::create_surface(const rhi::SurfaceDesc &desc) {
        WGPUSurfaceDescriptor surface_desc{};
        surface_desc.label = wgpu_string(desc.label);

        // Each window system has its own chained source struct. Only the three that pair with the
        // enabled Dawn backends are handled; the rest have no way to reach a Vulkan, Metal, or
        // D3D12 device in this build anyway.
        WGPUSurfaceSourceXlibWindow xlib{};
        WGPUSurfaceSourceWaylandSurface wayland{};
        WGPUSurfaceSourceWindowsHWND win32{};
        WGPUSurfaceSourceMetalLayer metal{};

        switch (desc.system) {
            case rhi::WindowSystem::Xlib:
                xlib.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
                xlib.display = desc.display;
                // Xlib window IDs are integers, not pointers; the RHI carries one in a void* slot.
                xlib.window = static_cast<u64>(reinterpret_cast<uintptr_t>(desc.window));
                surface_desc.nextInChain = &xlib.chain;
                break;
            case rhi::WindowSystem::Wayland:
                wayland.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
                wayland.display = desc.display;
                wayland.surface = desc.window;
                surface_desc.nextInChain = &wayland.chain;
                break;
            case rhi::WindowSystem::Win32:
                win32.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
                win32.hinstance = desc.display;
                win32.hwnd = desc.window;
                surface_desc.nextInChain = &win32.chain;
                break;
            case rhi::WindowSystem::Cocoa:
                metal.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
                metal.layer = desc.window;
                surface_desc.nextInChain = &metal.chain;
                break;
            case rhi::WindowSystem::Xcb:
            case rhi::WindowSystem::Android:
            case rhi::WindowSystem::UIKit:
            case rhi::WindowSystem::Unknown:
                return std::unexpected(webgpu_error(
                    "create_surface", "this window system has no surface source in this Dawn build"));
        }

        WGPUSurface surface = wgpuInstanceCreateSurface(instance_, &surface_desc);
        if (surface == nullptr) {
            return std::unexpected(webgpu_error("create_surface"));
        }
        return surfaces_.insert(SurfaceEntry{.surface = surface});
    }

    /// Destroys a surface.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_surface(rhi::SurfaceHandle handle) noexcept {
        surfaces_.erase(handle, [this](SurfaceEntry &entry) {
            if (entry.surface == nullptr) {
                return;
            }
            if (entry.configured) {
                wgpuSurfaceUnconfigure(entry.surface);
            }
            wgpuSurfaceRelease(entry.surface);

            // Releasing the surface does not destroy the swapchain or the platform surface behind
            // it. Dawn hands both to its fenced deleter, which queues them against the *pending*
            // command serial -- one past the last submitted -- so no amount of waiting retires
            // them; only a further submission does. Left alone they are collected when the device
            // is destroyed, which is far too late: the window this surface belongs to closes
            // moments from now, and destroying a Wayland-backed swapchain after its wl_surface is
            // gone marshals calls on freed memory.
            //
            // Submitting nothing at all is what advances the serial past those deletions, after
            // which draining and ticking runs them while the window is still alive.
            flush_deferred_deletions();
        });
    }

    /// Advances the queue by one empty submission, then drains and ticks so Dawn's fenced deleter
    /// actually runs the deletions it has queued against the pending serial.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::flush_deferred_deletions() noexcept {
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, nullptr);
        if (encoder != nullptr) {
            WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
            wgpuCommandEncoderRelease(encoder);
            if (commands != nullptr) {
                wgpuQueueSubmit(queue_, 1, &commands);
                wgpuCommandBufferRelease(commands);
            }
        }
        wait_idle();
        wgpuDeviceTick(device_);
    }

    /// Configures a surface for presentation and returns the swapchain naming that configuration.
    ///
    /// @param desc `desc` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SwapchainHandle> WebGpuDevice::create_swapchain(const rhi::SwapchainDesc &desc) {
        SurfaceEntry *entry = surfaces_.find(desc.surface);
        if (entry == nullptr) {
            return std::unexpected(webgpu_error("create_swapchain", "unknown surface handle"));
        }
        const WGPUTextureFormat format = to_wgpu(desc.format);
        if (format == WGPUTextureFormat_Undefined) {
            return std::unexpected(webgpu_error("create_swapchain", "the requested format has no WebGPU equivalent"));
        }

        WGPUSurfaceConfiguration config{};
        config.device = device_;
        config.format = format;
        config.usage = to_wgpu(desc.usage);
        config.width = desc.width;
        config.height = desc.height;
        // The RHI describes presentation as a strategy rather than a mode; WebGPU only has modes,
        // so the strategy is mapped onto the closest one it offers.
        switch (desc.present_strategy) {
            case rhi::PresentStrategy::Unsynchronized: config.presentMode = WGPUPresentMode_Immediate; break;
            case rhi::PresentStrategy::TearFreeLatest: config.presentMode = WGPUPresentMode_Mailbox; break;
            case rhi::PresentStrategy::AdaptiveTearing:
                // WebGPU has no adaptive/late-tearing mode; FIFO is the tear-free fallback.
            case rhi::PresentStrategy::TearFreeOrdered:
            default: config.presentMode = WGPUPresentMode_Fifo; break;
        }
        config.alphaMode = WGPUCompositeAlphaMode_Auto;
        // WebGPU does not let a caller pick an image count: the implementation chooses how deep to
        // make the chain, so desc.image_count has nothing to map to.

        wgpuSurfaceConfigure(entry->surface, &config);
        entry->format = desc.format;
        entry->width = desc.width;
        entry->height = desc.height;
        entry->configured = true;

        const rhi::SwapchainHandle swapchain = swapchains_.insert(rhi::SurfaceHandle{desc.surface});
        // This configuration now belongs to the new swapchain, so destroying whichever one held it
        // before leaves the surface alone.
        entry->configured_by = swapchain;
        return swapchain;
    }

    /// Destroys a swapchain, unconfiguring the surface behind it.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuDevice::destroy_swapchain(rhi::SwapchainHandle handle) noexcept {
        rhi::SurfaceHandle *surface_handle = swapchains_.find(handle);
        if (surface_handle != nullptr) {
            SurfaceEntry *entry = surfaces_.find(*surface_handle);
            // Only the swapchain that owns the surface's current configuration may tear it down. A
            // caller that recreated a swapchain has already reconfigured the surface for the
            // replacement, and unconfiguring here would leave that replacement unusable.
            if (entry != nullptr && entry->configured && entry->configured_by.value == handle.value) {
                wgpuSurfaceUnconfigure(entry->surface);
                entry->configured = false;
                entry->configured_by = rhi::SwapchainHandle{};
            }
        }
        swapchains_.erase(handle, [](rhi::SurfaceHandle &) {});
    }

    /// Reports how presentation was actually resolved for a swapchain.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    rhi::PresentationResolution WebGpuDevice::presentation_resolution(rhi::SwapchainHandle handle) const noexcept {
        (void)handle;
        // WebGPU does not report back what the implementation chose, so this describes what the
        // API guarantees rather than what a driver picked: tear-free ordered presentation, never
        // through a compute queue (there is only one queue), and opaque composition.
        return rhi::PresentationResolution{
            .strategy = rhi::PresentStrategy::TearFreeOrdered,
            .effective_mode = rhi::PresentMode::Fifo,
            .degraded = false,
            .present_queue_is_compute = false,
            .effective_composite_alpha = rhi::CompositeAlphaMode::Opaque,
            .composite_alpha_degraded = false,
            .via_composition_present = false,
        };
    }

    /// Queries a surface's HDR capabilities.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the error alternative; WebGPU exposes no HDR metadata.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> WebGpuDevice::query_hdr_capabilities(
        rhi::SurfaceHandle handle) const {
        (void)handle;
        // WebGPU has no equivalent of VK_EXT_hdr_metadata or DXGI's colour-space queries; a
        // surface reports supported formats but nothing about display luminance.
        return std::unexpected(unsupported_by_webgpu("HDR display capability queries"));
    }

    /// Updates a swapchain's HDR content light level metadata.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param update `update` value used by the operation.
    ///
    /// @return Returns the error alternative; WebGPU exposes no HDR metadata.
    /// @note Normal failures are returned through the type-specific error/status state.
    rhi::RhiResult WebGpuDevice::update_hdr_content_light_level(
        rhi::SwapchainHandle handle, const rhi::HdrContentLightLevelUpdate &update) {
        (void)handle;
        (void)update;
        return std::unexpected(unsupported_by_webgpu("HDR content light level metadata"));
    }

    /// Acquires the surface texture to render this frame into.
    ///
    /// @param swapchain `swapchain` value used by the operation.
    /// @param frame_slot_index `frame_slot_index` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::SurfaceTexture> WebGpuDevice::acquire_next_texture(rhi::SwapchainHandle swapchain,
                                                                            u32 frame_slot_index) {
        (void)frame_slot_index;
        rhi::SurfaceHandle *surface_handle = swapchains_.find(swapchain);
        if (surface_handle == nullptr) {
            return std::unexpected(webgpu_error("acquire_next_texture", "unknown swapchain handle"));
        }
        SurfaceEntry *entry = surfaces_.find(*surface_handle);
        if (entry == nullptr || !entry->configured) {
            return std::unexpected(webgpu_error("acquire_next_texture", "the surface is not configured"));
        }

        WGPUSurfaceTexture surface_texture{};
        wgpuSurfaceGetCurrentTexture(entry->surface, &surface_texture);
        switch (surface_texture.status) {
            case WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal:
                break;
            case WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal:
                // Still usable this frame; the caller is expected to reconfigure at its convenience.
                break;
            case WGPUSurfaceGetCurrentTextureStatus_Timeout:
            case WGPUSurfaceGetCurrentTextureStatus_Outdated:
            case WGPUSurfaceGetCurrentTextureStatus_Lost:
                return std::unexpected(rhi::RhiError{
                    .code = rhi::RhiErrorCode::SurfaceLost,
                    .message = "WebGPU: the surface is outdated and must be reconfigured.",
                });
            default:
                return std::unexpected(webgpu_error("acquire_next_texture"));
        }

        const bool suboptimal =
            surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal;

        // The texture's *storage* belongs to the surface and must never be destroyed, but the
        // reference wgpuSurfaceGetCurrentTexture just returned belongs to this caller and has to be
        // released like any other. `owned_by_surface` distinguishes the two: release, never destroy.
        // Getting that wrong leaks one texture reference per frame and leaves the surface believing
        // its texture is still in use.
        const rhi::TextureHandle texture_handle = textures_.insert(TextureEntry{
            .texture = surface_texture.texture,
            .format = entry->format,
            .extent = rhi::Extent3D{.width = entry->width, .height = entry->height, .depth_or_layers = 1},
            .mip_levels = 1,
            .owned_by_surface = true,
        });

        WGPUTextureViewDescriptor view_desc{};
        view_desc.label = wgpu_string("swapchain view");
        view_desc.format = to_wgpu(entry->format);
        view_desc.dimension = WGPUTextureViewDimension_2D;
        view_desc.mipLevelCount = 1;
        view_desc.arrayLayerCount = 1;
        view_desc.aspect = WGPUTextureAspect_All;

        WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, &view_desc);
        if (view == nullptr) {
            return std::unexpected(webgpu_error("acquire_next_texture", "could not create the surface texture view"));
        }

        return rhi::SurfaceTexture{
            .swapchain = swapchain,
            .texture = texture_handle,
            .view = texture_views_.insert(std::move(view)),
            .image_index = 0,
            .suboptimal = suboptimal,
        };
    }

    /// Presents a previously acquired surface texture.
    ///
    /// @param desc `desc` value used by the operation.
    /// @param queue_lock_wait_ms Optional out-parameter for queue lock contention; always zero here.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    rhi::RhiExpected<rhi::PresentOutcome> WebGpuDevice::present(const rhi::PresentDesc &desc,
                                                                f64 *queue_lock_wait_ms) {
        if (queue_lock_wait_ms != nullptr) {
            // There is one queue and no lock around it in this backend, so there is never any wait
            // to report.
            *queue_lock_wait_ms = 0.0;
        }

        rhi::SurfaceHandle *surface_handle = swapchains_.find(desc.texture.swapchain);
        if (surface_handle == nullptr) {
            return std::unexpected(webgpu_error("present", "unknown swapchain handle"));
        }
        SurfaceEntry *entry = surfaces_.find(*surface_handle);
        if (entry == nullptr) {
            return std::unexpected(webgpu_error("present", "unknown surface handle"));
        }

        wgpuSurfacePresent(entry->surface);

        // The acquired texture and its view are invalid after the present, so their handles are
        // retired here rather than left for the caller to destroy. The texture is released -- not
        // destroyed -- because the surface owns its storage but this backend owns the reference
        // acquire_next_texture took out.
        texture_views_.erase(desc.texture.view, [](WGPUTextureView &view) { wgpuTextureViewRelease(view); });
        textures_.erase(desc.texture.texture, [](TextureEntry &entry) {
            if (entry.texture != nullptr) {
                wgpuTextureRelease(entry.texture);
            }
        });

        if (desc.completion_fence.value != 0) {
            if (bool *signaled = fences_.find(desc.completion_fence); signaled != nullptr) {
                *signaled = true;
            }
        }
        return desc.texture.suboptimal ? rhi::PresentOutcome::Suboptimal : rhi::PresentOutcome::Success;
    }

} // namespace SFT::Core::WebGpu
