#pragma once

#include <Core/EngineBackend.hpp>

#pragma region Imports
#include <memory>
#include <optional>
#include <unordered_map>
#pragma endregion

namespace SFT::Core::WebGpu {

    /// The engine-level graphics backend that runs the renderer on WebGPU.
    ///
    /// Structurally the same shape as `D3D12Backend`: it owns the RHI instance, adapter selection,
    /// and device, and hands the renderer one `RHI::SurfaceHandle` per window. Swapchain creation,
    /// resize, and presentation all live in `Renderer`, which drives them through the RHI — so
    /// nothing here needs to know about frames in flight.
    ///
    /// The device underneath is Dawn talking to Vulkan, Metal, or D3D12 (see `sturdy_fetch_dawn`).
    class WebGpuBackend final : public EngineBackend {
      public:
        /// Destroys the backend and everything it owns.
        ///
        /// @note This function does not throw exceptions.
        ~WebGpuBackend() override;

        /// Brings up the instance, adapter, and device, and creates the primary window's surface.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererExpected<RenderSurfaceHandle> initialize(const RendererCreateInfo &init) override;

        /// Creates a surface for an additional window.
        ///
        /// @param window `window` value used by the operation.
        /// @param desired_frames_in_flight Ignored; the renderer owns swapchain depth.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererExpected<RenderSurfaceHandle> create_window_surface(
            WindowManager::Window &window,
            u32 desired_frames_in_flight = 2) override;

        /// Destroys a window's surface.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_window_surface(RenderSurfaceHandle surface) noexcept override;

        /// Notes that a surface needs to be resized.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param extent `extent` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept override;

        /// Returns what this backend can do.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererCapabilities capabilities() const noexcept override;

        /// Returns how the renderer may thread work on this backend.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::RenderThreadingCapabilities render_threading_capabilities() const noexcept override;

        /// Returns the RHI device.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept override;

        /// Returns the RHI device.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RHI::RhiDevice *rhi_device() const noexcept override;

        /// Returns the RHI surface backing a render surface.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RendererExpected<RHI::SurfaceHandle> rhi_surface_for(RenderSurfaceHandle surface) override;

        /// Describes the selected GPU.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] optional<GpuInfo> gpu_info() const override;

        /// Waits until the device has finished all submitted work.
        ///
        /// @note This function does not throw exceptions.
        void wait_idle() noexcept override;

      private:
        friend class ::SFT::Core::EngineBackend;

        /// Constructs the backend. Only `EngineBackend::create` may call this.
        ///
        /// @param key Construction key proving the call came through the factory.
        ///
        /// @note This function does not throw exceptions.
        explicit WebGpuBackend(ConstructorKey key);

        /// Creates the RHI surface for `window` and records it against the window's id.
        ///
        /// @param window `window` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RendererExpected<RenderSurfaceHandle> create_surface(WindowManager::Window &window);

        /// Releases every surface, the device, and the instance.
        ///
        /// @note This function does not throw exceptions.
        void destroy_resources() noexcept;

        /// Fills `capabilities_` from what the device actually negotiated.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void populate_capabilities(const RendererCreateInfo &init) noexcept;

        RendererCapabilities capabilities_{};
        optional<RHI::AdapterInfo> adapter_info_{};
        std::unordered_map<WindowManager::WindowId, RHI::SurfaceHandle> surfaces_;
        std::unique_ptr<RHI::RhiInstance> instance_;
        std::unique_ptr<RHI::RhiDevice> device_;
        // Set once the Dawn device has been explicitly destroyed, which this backend does as soon
        // as its last surface goes away rather than waiting for its own destructor. See
        // `destroy_window_surface` for why; the flag is what keeps a later `create_surface` from
        // handing work to a device that is already gone.
        bool device_destroyed_ = false;
        bool initialized_ = false;
    };

} // namespace SFT::Core::WebGpu
