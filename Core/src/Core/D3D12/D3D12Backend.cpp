#include <Core/D3D12/D3D12Backend.hpp>

#include <D3D12/D3D12Adapter.hpp>

#pragma region Imports
#include <expected>
#include <format>
#include <ranges>
#include <utility>
#pragma endregion

#include <Core/GraphicsBackendError.hpp>

namespace SFT::Core::D3D12 {

    namespace {

        /// Converts the backend-specific value to the corresponding RHI representation.
        ///
        /// @param system `system` value used by the operation.
        ///
        /// @return Returns the value converted to RHI window system representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::WindowSystem to_rhi_window_system(Platform::Windowing::NativeWindowSystem system) noexcept {
            return system == Platform::Windowing::NativeWindowSystem::Win32
                ? RHI::WindowSystem::Win32
                : RHI::WindowSystem::Unknown;
        }

        /// Retrieves or produces the to graphics error code selected by the supplied arguments.
        ///
        /// @param code `code` value used by the operation.
        ///
        /// @return Returns the value converted to graphics error code representation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] GraphicsBackendErrorCode to_graphics_error_code(RHI::RhiErrorCode code) noexcept {
            switch (code) {
                case RHI::RhiErrorCode::Unsupported: return GraphicsBackendErrorCode::Unsupported;
                case RHI::RhiErrorCode::OutOfMemory: return GraphicsBackendErrorCode::OutOfMemory;
                case RHI::RhiErrorCode::DeviceLost: return GraphicsBackendErrorCode::DeviceLost;
                case RHI::RhiErrorCode::SurfaceLost: return GraphicsBackendErrorCode::SurfaceLost;
                case RHI::RhiErrorCode::FullScreenExclusiveLost: return GraphicsBackendErrorCode::FullScreenExclusiveLost;
                case RHI::RhiErrorCode::OperationFailed:
                case RHI::RhiErrorCode::NotReady:
                case RHI::RhiErrorCode::InvalidArgument: return GraphicsBackendErrorCode::OperationFailed;
            }
            return GraphicsBackendErrorCode::OperationFailed;
        }

    } // namespace

    /// Performs the d3 d12 backend operation for `D3D12` using the supplied arguments.
    ///
    /// @param key Key used to identify the requested entry.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    D3D12Backend::D3D12Backend(ConstructorKey key)
        : EngineBackend(key) {}

    /// Destroys the `D3D12` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    D3D12Backend::~D3D12Backend() { destroy_resources(); }

    /// Initializes the `D3D12` for use.
    ///
    /// @param init `init` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`, `GraphicsBackendErrorCode::Unsupported`.
    RendererExpected<RenderSurfaceHandle> D3D12Backend::initialize(const RendererCreateInfo &init) {
        if (initialized_) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "D3D12 backend is already initialized.");
        }
        if (init.window == nullptr) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "D3D12 backend requires a primary Win32 window.");
        }

        const RHI::InstanceDesc instance_desc{
            .application_name = init.app_name,
#if defined(DEBUG) || defined(_DEBUG)
            .enable_validation = true,
            .enable_debug_utils = true,
#endif
        };
        auto instance = ::SFT::D3D12::create_d3d12_instance(instance_desc);
        if (!instance) [[unlikely]] {
            return graphics_backend_error(to_graphics_error_code(instance.error().code),
                                          std::format("Failed to create D3D12 instance: {}", instance.error().message));
        }

        auto adapters = (*instance)->enumerate_adapters();
        if (!adapters) [[unlikely]] {
            return graphics_backend_error(to_graphics_error_code(adapters.error().code),
                                          std::format("Failed to enumerate D3D12 adapters: {}", adapters.error().message));
        }
        if (adapters->empty()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "D3D12 instance enumerated no adapters.");
        }

        RHI::FeatureSet optional_features = init.features.optional_rhi_features;
        if (init.features.raytracing) {
            optional_features.set(RHI::Feature::RayTracingPipeline)
                .set(RHI::Feature::RayQuery)
                .set(RHI::Feature::AccelerationStructures)
                .set(RHI::Feature::BufferDeviceAddress)
                .set(RHI::Feature::BindlessResources);
        }
        const RHI::DeviceRequest device_request{
            .required_features = init.features.required_rhi_features,
            .optional_features = optional_features,
            .required_extensions = {},
            .optional_extensions = {},
            .queue_requests = {},
            .label = "SturdyEngine D3D12 device",
        };


        const auto selected = std::ranges::find_if(*adapters, [&](const auto &candidate) {
            return candidate->supported_features().contains_all(device_request.required_features) &&
                   (init.physical_device_id.empty() || candidate->info().physical_device_id == init.physical_device_id);
        });
        if (selected == adapters->end()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "No selected D3D12 adapter satisfies the required RHI features.");
        }
        RHI::RhiAdapter &adapter = **selected;
        auto device = adapter.create_device(device_request);
        if (!device) [[unlikely]] {
            return graphics_backend_error(to_graphics_error_code(device.error().code),
                                          std::format("Failed to create D3D12 device: {}", device.error().message));
        }

        instance_ = std::move(*instance);
        device_ = std::move(*device);
        adapter_info_ = adapter.info();
        initialized_ = true;
        populate_capabilities(init);

        auto primary_surface = create_surface(*init.window);
        if (!primary_surface) [[unlikely]] {
            const GraphicsBackendError error = primary_surface.error();
            destroy_resources();
            return std::unexpected(error);
        }
        return primary_surface;
    }

    /// Creates a surface from the supplied parameters.
    ///
    /// @param window Window used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`, `GraphicsBackendErrorCode::Unsupported`.
    RendererExpected<RenderSurfaceHandle> D3D12Backend::create_surface(Platform::Windowing::Window &window) {
        if (!initialized_ || !device_) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "D3D12 backend must be initialized before creating a surface.");
        }
        const auto native = window.native_window_handle();
        if (!native) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          std::format("Failed to query native window handle for D3D12 surface: {}", native.error().message));
        }
        if (to_rhi_window_system(native->system) != RHI::WindowSystem::Win32 || native->window == nullptr) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          "D3D12 backend only supports Win32 window surfaces.");
        }

        const Platform::Windowing::WindowId window_id = window.id();
        if (surfaces_.contains(window_id)) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "A D3D12 surface already exists for this window.");
        }

        auto surface = device_->create_surface(RHI::SurfaceDesc{
            .system = RHI::WindowSystem::Win32,
            .display = native->display,
            .window = native->window,
        });
        if (!surface) [[unlikely]] {
            return graphics_backend_error(to_graphics_error_code(surface.error().code),
                                          std::format("Failed to create D3D12 Win32 surface: {}", surface.error().message));
        }
        surfaces_.emplace(window_id, *surface);
        return RenderSurfaceHandle{window_id};
    }

    /// Creates a window surface from the supplied parameters.
    ///
    /// @param window Window used or affected by the operation.
    /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RendererExpected<RenderSurfaceHandle> D3D12Backend::create_window_surface(
        Platform::Windowing::Window &window,
        u32 desired_frames_in_flight) {
        (void)desired_frames_in_flight;
        return create_surface(window);
    }

    /// Destroys the window surface identified by the supplied parameters.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Backend::destroy_window_surface(RenderSurfaceHandle surface) noexcept {
        const auto it = surfaces_.find(surface.window_id);
        if (it == surfaces_.end()) {
            return;
        }
        if (device_) {
            device_->destroy_surface(it->second);
        }
        surfaces_.erase(it);
    }

    /// Handles the on surface resize needed callback and updates the associated platform state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Backend::on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept {
        (void)surface;
        (void)extent;

    }

    /// Returns the current or globally available capabilities value.
    ///
    /// @return Returns the current capabilities value.
    /// @note This function does not throw exceptions.
    RendererCapabilities D3D12Backend::capabilities() const noexcept { return capabilities_; }

    /// Returns the current render threading capabilities.
    ///
    /// @return Returns the current render threading capabilities value.
    /// @note This function does not throw exceptions.
    RHI::RenderThreadingCapabilities D3D12Backend::render_threading_capabilities() const noexcept {
        return RHI::RenderThreadingCapabilities{
            .backend_allows_dedicated_render_thread = true,
            .backend_allows_parallel_command_recording = true,
            .platform_allows_threads = RHI::compile_time_rhi_multithreading_allowed,
            .requires_graphics_calls_on_owner_thread = true,
            .recommended_mode = RHI::choose_render_threading_mode(RHI::RenderThreadingCapabilities{
                .backend_allows_dedicated_render_thread = true,
                .backend_allows_parallel_command_recording = true,
                .platform_allows_threads = RHI::compile_time_rhi_multithreading_allowed,
                .requires_graphics_calls_on_owner_thread = true,
            }),
        };
    }

    /// Returns the current or globally available RHI device value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    RHI::RhiDevice *D3D12Backend::rhi_device() noexcept { return device_.get(); }

    /// Returns the current or globally available RHI device value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    const RHI::RhiDevice *D3D12Backend::rhi_device() const noexcept { return device_.get(); }

    /// Resolves the RHI surface associated with the supplied key, handle, or resource.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    RendererExpected<RHI::SurfaceHandle> D3D12Backend::rhi_surface_for(RenderSurfaceHandle surface) {
        const auto it = surfaces_.find(surface.window_id);
        if (it == surfaces_.end()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                          "Cannot get an RHI surface for an unknown D3D12 surface.");
        }
        return it->second;
    }

    /// Returns the current GPU info.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<GpuInfo> D3D12Backend::gpu_info() const {
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

    /// Waits for idle to complete.
    ///
    /// @return Returns the current wait idle value.
    /// @note This function does not throw exceptions.
    void D3D12Backend::wait_idle() noexcept {
        if (device_) {
            device_->wait_idle();
        }
    }

    /// Destroys the resources identified by the supplied parameters.
    ///
    /// @return Returns the current destroy resources value.
    /// @note This function does not throw exceptions.
    void D3D12Backend::destroy_resources() noexcept {
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

    /// Removes and returns or discards the next value from the container or queue.
    ///
    /// @param init `init` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void D3D12Backend::populate_capabilities(const RendererCreateInfo &init) noexcept {
        const RHI::FeatureSet &enabled = device_->enabled_features();
        capabilities_.multithreaded_command_recording = true;
        capabilities_.async_compute = enabled.has(RHI::Feature::AsyncCompute);
        capabilities_.raytracing = enabled.has(RHI::Feature::RayTracingPipeline) || enabled.has(RHI::Feature::RayQuery);
        capabilities_.mesh_shaders = enabled.has(RHI::Feature::MeshShader);
        capabilities_.bindless = enabled.has(RHI::Feature::BindlessResources);
        capabilities_.timeline_semaphores = enabled.has(RHI::Feature::TimelineSynchronization);
        const auto resolution = resolve_frames_in_flight(init.features.desired_frames_in_flight, 2, 0);
        capabilities_.max_frames_in_flight = resolution ? resolution->resolved : 2;
    }

} // namespace SFT::Core::D3D12

namespace SFT::Core {

    /// Creates a D3D12 backend from the supplied parameters.
    ///
    /// @return Returns the current create D3D12 backend value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    unique_ptr<EngineBackend> create_d3d12_backend() {
        return EngineBackend::create_erased<D3D12::D3D12Backend>();
    }

} // namespace SFT::Core
