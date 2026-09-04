#include <Core/WebGPU/WebGpuBackend.hpp>

#include <Core/WebGPU/RHI/WebGpuDevice.hpp>

#include <Foundation/Foundation.hpp>

#include <Core/GraphicsBackendError.hpp>
#include <Core/WebGPU/RHI/WebGpuAdapter.hpp>

#pragma region Imports
#include <algorithm>
#include <expected>
#include <format>
#include <ranges>
#include <utility>
#pragma endregion

namespace SFT::Core::WebGpu {

    namespace {

        /// Maps a window manager's native window system onto the RHI's.
        ///
        /// All four are accepted rather than the single one D3D12 allows, because Dawn reaches
        /// Vulkan on X11 and Wayland, Metal on Cocoa, and D3D12 on Win32 — the same three drivers
        /// this build enables, across every windowing system they appear on.
        ///
        /// @param system `system` value used by the operation.
        ///
        /// @return Returns the value converted to the RHI representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::WindowSystem to_rhi_window_system(WindowManager::NativeWindowSystem system) noexcept {
            switch (system) {
                case WindowManager::NativeWindowSystem::Win32: return RHI::WindowSystem::Win32;
                case WindowManager::NativeWindowSystem::X11: return RHI::WindowSystem::Xlib;
                case WindowManager::NativeWindowSystem::Wayland: return RHI::WindowSystem::Wayland;
                case WindowManager::NativeWindowSystem::Cocoa: return RHI::WindowSystem::Cocoa;
                case WindowManager::NativeWindowSystem::Unknown: break;
            }
            return RHI::WindowSystem::Unknown;
        }

        /// Maps an RHI error code onto the graphics-backend one the engine layer reports.
        ///
        /// @param code `code` value used by the operation.
        ///
        /// @return Returns the value converted to the graphics-backend representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] GraphicsBackendErrorCode to_graphics_error_code(RHI::RhiErrorCode code) noexcept {
            switch (code) {
                case RHI::RhiErrorCode::Unsupported: return GraphicsBackendErrorCode::Unsupported;
                case RHI::RhiErrorCode::OutOfMemory: return GraphicsBackendErrorCode::OutOfMemory;
                case RHI::RhiErrorCode::DeviceLost: return GraphicsBackendErrorCode::DeviceLost;
                case RHI::RhiErrorCode::SurfaceLost: return GraphicsBackendErrorCode::SurfaceLost;
                case RHI::RhiErrorCode::FullScreenExclusiveLost:
                    return GraphicsBackendErrorCode::FullScreenExclusiveLost;
                case RHI::RhiErrorCode::OperationFailed:
                case RHI::RhiErrorCode::NotReady:
                case RHI::RhiErrorCode::InvalidArgument:
                    return GraphicsBackendErrorCode::OperationFailed;
            }
            return GraphicsBackendErrorCode::OperationFailed;
        }

    } // namespace

    WebGpuBackend::WebGpuBackend(ConstructorKey key) : EngineBackend(key) {}

    WebGpuBackend::~WebGpuBackend() { destroy_resources(); }

    /// Brings up the instance, adapter, and device, and creates the primary window's surface.
    ///
    /// @param init `init` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RendererExpected<RenderSurfaceHandle> WebGpuBackend::initialize(const RendererCreateInfo &init) {
        if (initialized_) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "WebGPU backend is already initialized.");
        }
        if (init.window == nullptr) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "WebGPU backend requires a window to create its primary surface.");
        }

        const RHI::InstanceDesc instance_desc{
            .application_name = init.app_name,
#if defined(DEBUG) || defined(_DEBUG)
            .enable_validation = true,
            .enable_debug_utils = true,
#endif
        };

        const RHI::BackendRegistration registration = webgpu_backend_registration();
        auto instance = registration.create_instance(instance_desc);
        if (!instance) [[unlikely]] {
            return graphics_backend_error(
                to_graphics_error_code(instance.error().code),
                std::format("Failed to create WebGPU instance: {}", instance.error().message));
        }

        auto adapters = (*instance)->enumerate_adapters();
        if (!adapters) [[unlikely]] {
            return graphics_backend_error(
                to_graphics_error_code(adapters.error().code),
                std::format("Failed to enumerate WebGPU adapters: {}", adapters.error().message));
        }
        if (adapters->empty()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "The WebGPU instance enumerated no adapters.");
        }

        // No optional RHI features are requested. WebGPU has none of the ones the other backends
        // negotiate -- no ray tracing, mesh shaders, bindless descriptor indexing, timeline
        // semaphores, variable-rate shading, or depth bounds -- so asking for them would only
        // produce a device request that cannot be satisfied. Anything the caller marked *required*
        // is still honoured below, which is what makes a renderer that genuinely needs one of them
        // fail here with a clear message rather than midway through a frame.
        const RHI::DeviceRequest device_request{
            .required_features = init.features.required_rhi_features,
            .label = "Sturdy WebGPU device",
        };

        // WebGPU adapters carry no stable device id (see WebGpuAdapter.cpp), so a caller pinning a
        // specific GPU is matched on the name the inventory showed them instead.
        const auto matches_request = [&init](const std::unique_ptr<RHI::RhiAdapter> &candidate) {
            if (candidate == nullptr) {
                return false;
            }
            if (init.physical_device_id.empty()) {
                return true;
            }
            return candidate->info().physical_device_id == init.physical_device_id ||
                   candidate->info().name == init.physical_device_id;
        };

        auto selected = std::ranges::find_if(*adapters, matches_request);
        if (selected == adapters->end()) [[unlikely]] {
            // Falling back rather than failing: the requested GPU was named by another backend's
            // inventory entry, and refusing to start because WebGPU cannot match that name would be
            // worse than running on the adapter it did find.
            selected = adapters->begin();
        }

        RHI::RhiAdapter &adapter = **selected;
        auto device = adapter.create_device(device_request);
        if (!device) [[unlikely]] {
            return graphics_backend_error(
                to_graphics_error_code(device.error().code),
                std::format("Failed to create WebGPU device: {}", device.error().message));
        }

        instance_ = std::move(*instance);
        device_ = std::move(*device);
        adapter_info_ = adapter.info();
        initialized_ = true;
        populate_capabilities(init);

        Foundation::log_info("WebGPU backend initialized on {} [{}]", adapter_info_->name,
                             adapter_info_->api_version);

        auto primary_surface = create_surface(*init.window);
        if (!primary_surface) [[unlikely]] {
            const GraphicsBackendError error = primary_surface.error();
            destroy_resources();
            return std::unexpected(error);
        }
        return primary_surface;
    }

    /// Creates the RHI surface for `window` and records it against the window's id.
    ///
    /// @param window `window` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RendererExpected<RenderSurfaceHandle> WebGpuBackend::create_surface(WindowManager::Window &window) {
        if (device_destroyed_) [[unlikely]] {
            return graphics_backend_error(
                GraphicsBackendErrorCode::InitializationFailed,
                "The WebGPU device was destroyed when this backend's last surface went away and "
                "cannot take another; create a new backend instead.");
        }
        if (!initialized_ || !device_) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "WebGPU backend must be initialized before creating a surface.");
        }

        const auto native = window.native_window_handle();
        if (!native) [[unlikely]] {
            return graphics_backend_error(
                GraphicsBackendErrorCode::InitializationFailed,
                std::format("Failed to query native window handle for a WebGPU surface: {}",
                            native.error().message));
        }

        const RHI::WindowSystem system = to_rhi_window_system(native->system);
        if (system == RHI::WindowSystem::Unknown || native->window == nullptr) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "This window system has no WebGPU surface source.");
        }

        const WindowManager::WindowId window_id = window.id();
        if (surfaces_.contains(window_id)) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "A WebGPU surface already exists for this window.");
        }

        auto surface = device_->create_surface(RHI::SurfaceDesc{
            .system = system,
            .display = native->display,
            .window = native->window,
            .label = "Sturdy WebGPU surface",
        });
        if (!surface) [[unlikely]] {
            return graphics_backend_error(
                to_graphics_error_code(surface.error().code),
                std::format("Failed to create the WebGPU surface: {}", surface.error().message));
        }

        surfaces_.emplace(window_id, *surface);
        Foundation::log_info("WebGPU surface created for window {}.", static_cast<usize>(window_id));
        return RenderSurfaceHandle{window_id};
    }

    /// Creates a surface for an additional window.
    ///
    /// @param window `window` value used by the operation.
    /// @param desired_frames_in_flight Ignored; the renderer owns swapchain depth.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RendererExpected<RenderSurfaceHandle> WebGpuBackend::create_window_surface(WindowManager::Window &window,
                                                                               u32 desired_frames_in_flight) {
        // WebGPU chooses its own swapchain depth when a surface is configured; there is nothing to
        // pass this along to.
        (void)desired_frames_in_flight;
        return create_surface(window);
    }

    /// Destroys a window's surface.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuBackend::destroy_window_surface(RenderSurfaceHandle surface) noexcept {
        const auto it = surfaces_.find(surface.window_id);
        if (it == surfaces_.end()) {
            return;
        }
        if (device_) {
            device_->destroy_surface(it->second);
        }
        surfaces_.erase(it);
        Foundation::log_info("WebGPU surface destroyed for window {}.", static_cast<u64>(surface.window_id));

        // With the last surface gone, destroy the Dawn device now rather than leaving it to this
        // backend's own destructor.
        //
        // Dawn defers destroying a swapchain's Vulkan objects to its fenced deleter, and only
        // guarantees that deleter is drained when the device is destroyed. A swapchain on Wayland
        // talks to the compositor as it is destroyed, so if that is left until the backend unwinds
        // -- which happens after the application has already closed its windows -- the calls land
        // on a wl_surface that no longer exists and the driver faults. Destroying the device here,
        // while the window is still up, is what makes that teardown legal.
        //
        // Nothing can be rendered without a surface anyway, so this costs no working state; a
        // caller that comes back for another surface gets the explicit error in `create_surface`
        // rather than a device that has quietly stopped functioning.
        if (surfaces_.empty() && device_ && !device_destroyed_) {
            wgpuDeviceDestroy(static_cast<WebGpuDevice *>(device_.get())->wgpu_device());
            device_destroyed_ = true;
        }
    }

    /// Notes that a surface needs to be resized.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param extent `extent` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuBackend::on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept {
        // Nothing to do here, exactly as on D3D12: the renderer responds to a resize by recreating
        // its swapchain through the RHI, which reconfigures the surface with the new size.
        (void)surface;
        (void)extent;
    }

    /// Returns what this backend can do.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    RendererCapabilities WebGpuBackend::capabilities() const noexcept { return capabilities_; }

    /// Returns how the renderer may thread work on this backend.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    RHI::RenderThreadingCapabilities WebGpuBackend::render_threading_capabilities() const noexcept {
        // A dedicated render thread is fine -- Dawn's objects are internally synchronised. Parallel
        // command *recording* is deliberately off: WebGPU has no secondary command buffers, and
        // every encoder here writes into one device-owned encoder, so recording several passes at
        // once would serialise inside Dawn anyway while adding contention. It can be revisited once
        // the backend is doing real frames and there is something to measure.
        const RHI::RenderThreadingCapabilities capabilities{
            .backend_allows_dedicated_render_thread = true,
            .backend_allows_parallel_command_recording = false,
            // Presentation must not overlap the render thread. WebGPU has one queue, and Dawn's
            // Vulkan backend records presentation's image transition into the very same pending
            // command buffer the render thread is submitting through -- running the two at once
            // has them writing one VkCommandPool from two threads, which ends in the driver
            // dereferencing a command buffer that another thread already ended.
            .backend_allows_async_presentation = false,
            .platform_allows_threads = RHI::compile_time_rhi_multithreading_allowed,
            .requires_graphics_calls_on_owner_thread = false,
        };
        return RHI::RenderThreadingCapabilities{
            .backend_allows_dedicated_render_thread = capabilities.backend_allows_dedicated_render_thread,
            .backend_allows_parallel_command_recording = capabilities.backend_allows_parallel_command_recording,
            .backend_allows_async_presentation = capabilities.backend_allows_async_presentation,
            .platform_allows_threads = capabilities.platform_allows_threads,
            .requires_graphics_calls_on_owner_thread = capabilities.requires_graphics_calls_on_owner_thread,
            .recommended_mode = RHI::choose_render_threading_mode(capabilities),
        };
    }

    /// Returns the RHI device.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    RHI::RhiDevice *WebGpuBackend::rhi_device() noexcept { return device_.get(); }

    /// Returns the RHI device.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note This function does not throw exceptions.
    const RHI::RhiDevice *WebGpuBackend::rhi_device() const noexcept { return device_.get(); }

    /// Returns the RHI surface backing a render surface.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RendererExpected<RHI::SurfaceHandle> WebGpuBackend::rhi_surface_for(RenderSurfaceHandle surface) {
        const auto it = surfaces_.find(surface.window_id);
        if (it == surfaces_.end()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                          "Cannot get an RHI surface for an unknown WebGPU surface.");
        }
        return it->second;
    }

    /// Describes the selected GPU.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    optional<GpuInfo> WebGpuBackend::gpu_info() const {
        if (!adapter_info_) {
            return std::nullopt;
        }
        return GpuInfo{
            .name = adapter_info_->name,
            .vendor = adapter_info_->vendor,
            .driver_version = adapter_info_->driver_version,
            .api_version = adapter_info_->api_version,
            .device_type = RHI::device_type_name(adapter_info_->device_type),
            .vendor_id = adapter_info_->vendor_id,
            .device_id = adapter_info_->device_id,
        };
    }

    /// Waits until the device has finished all submitted work.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuBackend::wait_idle() noexcept {
        if (device_) {
            device_->wait_idle();
        }
    }

    /// Releases every surface, the device, and the instance.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuBackend::destroy_resources() noexcept {
        wait_idle();
        if (device_) {
            for (const RHI::SurfaceHandle surface : surfaces_ | std::views::values) {
                device_->destroy_surface(surface);
            }
        }
        surfaces_.clear();
        device_.reset();
        instance_.reset();
        adapter_info_.reset();
        initialized_ = false;
    }

    /// Fills `capabilities_` from what the device actually negotiated.
    ///
    /// @param init `init` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void WebGpuBackend::populate_capabilities(const RendererCreateInfo &init) noexcept {
        // Every one of these is false because WebGPU has no form of the feature at all, not because
        // this backend has not got to it yet. Reporting them honestly is what lets the renderer
        // pick its non-raytraced, non-bindless paths instead of failing at first use.
        capabilities_.multithreaded_command_recording = false;
        capabilities_.async_compute = false;
        capabilities_.raytracing = false;
        capabilities_.mesh_shaders = false;
        capabilities_.bindless = false;
        capabilities_.timeline_semaphores = false;

        const auto resolution = resolve_frames_in_flight(init.features.desired_frames_in_flight, 2, 0);
        capabilities_.max_frames_in_flight = resolution ? resolution->resolved : 2;
    }

} // namespace SFT::Core::WebGpu

namespace SFT::Core {

    /// Creates the WebGPU engine backend.
    ///
    /// @return Returns exclusive ownership of the created object.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    unique_ptr<EngineBackend> create_webgpu_backend() {
        return EngineBackend::create_erased<WebGpu::WebGpuBackend>();
    }

} // namespace SFT::Core
