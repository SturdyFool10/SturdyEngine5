#pragma once

#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <concepts>
#include <memory>
#include <optional>
#include <utility>
#pragma endregion

#include <WindowManager/WindowManager.hpp>
#include <RHI/RHI.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <Core/RenderSurface.hpp>

using std::derived_from;
using std::optional;
using std::shared_ptr;
using std::unique_ptr;

namespace SFT::Core {


    class EngineBackend {
      protected:
        struct ConstructorKey {
          private:
            friend class EngineBackend;
            /// Constructs a `ConstructorKey` in its default state.
            ///
            /// @note This function does not throw exceptions.
            constexpr ConstructorKey() = default;
        };

        /// Constructs a `EngineBackend` from the supplied initialization values.
        ///
        /// @note This function does not throw exceptions.
        explicit constexpr EngineBackend(ConstructorKey) noexcept {}

      public:


        /// Destroys the `EngineBackend` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        virtual ~EngineBackend() = default;

        /// Disables this construction form for `EngineBackend`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        EngineBackend(const EngineBackend &) = delete;
        /// Assigns a new value to this `EngineBackend`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        EngineBackend &operator=(const EngineBackend &) = delete;
        /// Disables this construction form for `EngineBackend`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        EngineBackend(EngineBackend &&) = delete;
        /// Assigns a new value to this `EngineBackend`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        EngineBackend &operator=(EngineBackend &&) = delete;


        /// Initializes the `EngineBackend` for use.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        virtual RendererExpected<RenderSurfaceHandle> initialize(const RendererCreateInfo &init) = 0;


        /// Creates a window surface from the supplied parameters.
        ///
        /// @param window Window used or affected by the operation.
        /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        virtual RendererExpected<RenderSurfaceHandle> create_window_surface(
            WindowManager::Window &window,
            u32 desired_frames_in_flight = 2) = 0;


        /// Destroys the window surface identified by the supplied parameters.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void destroy_window_surface(RenderSurfaceHandle surface) noexcept = 0;


        /// Handles the surface resize needed event.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param extent `extent` value used by the operation.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept = 0;


        /// Returns the current or globally available capabilities value.
        ///
        /// @return Returns the current capabilities value.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual RendererCapabilities capabilities() const noexcept = 0;


        /// Returns the current render threading capabilities.
        ///
        /// @return Returns the current render threading capabilities value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual RHI::RenderThreadingCapabilities render_threading_capabilities() const noexcept;


        /// Returns the current or globally available RHI device value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual RHI::RhiDevice *rhi_device() noexcept;
        /// Returns the current or globally available RHI device value.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note Absence is represented by a null pointer rather than an exception.
        /// @note This function does not throw exceptions.
        [[nodiscard]] virtual const RHI::RhiDevice *rhi_device() const noexcept;


        /// Resolves the RHI surface associated with the supplied key, handle, or resource.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`.
        [[nodiscard]] virtual RendererExpected<RHI::SurfaceHandle> rhi_surface_for(RenderSurfaceHandle surface);


        /// Returns the current GPU info.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        [[nodiscard]] virtual optional<GpuInfo> gpu_info() const = 0;


        /// Waits for idle to complete.
        ///
        /// @note Concrete implementations define backend-specific failure details and must honor this declaration's result/error contract.
        /// @note This function does not throw exceptions.
        virtual void wait_idle() noexcept = 0;

        /// Constructs the requested concrete engine-backend type and returns ownership to the caller.
        ///
        /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Backend, typename... Args>
            requires derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static unique_ptr<Backend> create(Args &&...args) {
            return unique_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }

        /// Creates an engine backend instance and returns it through the erased/base ownership type.
        ///
        /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Backend, typename... Args>
            requires derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static unique_ptr<EngineBackend> create_erased(Args &&...args) {
            return unique_ptr<EngineBackend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }

        /// Creates an engine backend instance with shared ownership.
        ///
        /// @return Returns shared ownership of the created object; it remains alive until the final shared owner releases it.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <typename Backend, typename... Args>
            requires derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static shared_ptr<Backend> create_shared(Args &&...args) {
            return shared_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }
    };


    /// Creates a vulkan backend from the supplied parameters.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] unique_ptr<EngineBackend> create_vulkan_backend();


    /// Creates a engine backend from the supplied parameters.
    ///
    /// @param backend Backend value to inspect, select, or convert.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] unique_ptr<EngineBackend> create_engine_backend(RHI::BackendType backend);

#if defined(STURDY_ENABLE_WEBGPU)

    /// Creates the WebGPU backend.
    ///
    /// Only declared when the engine was built with `STURDY_ENABLE_WEBGPU`; without Dawn there is no
    /// WebGPU implementation to construct.
    ///
    /// @return Returns exclusive ownership of the created object; destroying or resetting the returned pointer releases it.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] unique_ptr<EngineBackend> create_webgpu_backend();
#endif

#if defined(_WIN32)

    /// Creates a D3D12 backend from the supplied parameters.
    ///
    /// @return Returns the current create D3D12 backend value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] unique_ptr<EngineBackend> create_d3d12_backend();
#endif

} // namespace SFT::Core
