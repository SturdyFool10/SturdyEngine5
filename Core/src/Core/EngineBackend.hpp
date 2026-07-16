#pragma once

#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>

#pragma region Imports
#include <concepts>
#include <memory>
#include <optional>
#include <utility>
#pragma endregion

#include <Platform/Platform.hpp>
#include <RHI/RHI.hpp>
#include "GraphicsBackendError.hpp"
#include "Renderer.hpp"
#include "RenderSurface.hpp"

using std::derived_from;
using std::optional;
using std::shared_ptr;
using std::unique_ptr;

namespace SFT::Core {

    // The renderer abstraction seam. Every graphics API (Vulkan today; Metal, WebGPU later) derives
    // from this. The Engine layer speaks only this interface and the API-agnostic types in
    // Renderer.cppm / RenderSurface.cppm — no Vulkan symbols ever leak upward.
    //
    // The backend owns API/device state: instance, physical/logical device, queues, allocator, and the
    // RHI device bridge. The renderer owns frame orchestration and all rendering/presentation resources
    // through RHI, so additional graphics APIs plug in by implementing RHI rather than exposing a
    // backend-specific render loop.
    class EngineBackend {
      protected:
        struct ConstructorKey {
          private:
            friend class EngineBackend;
            constexpr ConstructorKey() = default;
        };

        explicit constexpr EngineBackend(ConstructorKey) noexcept {}

      public:
        // RAII contract — implementations MUST:
        //   1. Call wait_idle() to drain all in-flight GPU work.
        //   2. Destroy backend-side surfaces in reverse creation order.
        //   3. Destroy the device, allocator, debug layer, and instance.
        // There is no separate shutdown() call — the destructor is the only teardown path.
        virtual ~EngineBackend() = default;

        EngineBackend(const EngineBackend &) = delete;
        EngineBackend &operator=(const EngineBackend &) = delete;
        EngineBackend(EngineBackend &&) = delete;
        EngineBackend &operator=(EngineBackend &&) = delete;

        // Bring up backend-global GPU state and construct the initial backend-side surface used for
        // device/queue present-support selection. The returned handle is an opaque address for render
        // calls and resize-needed notifications; actual frame presentation resources are owned by RHI.
        virtual RendererExpected<RenderSurfaceHandle> initialize(const RendererCreateInfo &init) = 0;

        // Adds another window's backend-side surface to an already-initialized backend, sharing the
        // existing device/queue/allocator. RHI owns the matching presentation resources used by frames.
        virtual RendererExpected<RenderSurfaceHandle> create_window_surface(
            Platform::Windowing::Window &window,
            u32 desired_frames_in_flight = 2) = 0;

        // Destroys one window's backend-side surface. Safe to call for the primary surface returned
        // by initialize() as well as any surface returned by create_window_surface().
        virtual void destroy_window_surface(RenderSurfaceHandle surface) noexcept = 0;

        // Notify the backend that a surface needs resize handling. The renderer/RHI owns swapchain
        // recreation; this remains a backend hook for API-specific surface bookkeeping. `extent` is
        // the fresh framebuffer size already resolved on the window-owning thread — implementations
        // must use it as-is rather than querying the Window themselves: this hook typically runs on
        // a dedicated render thread, and Window is not safe to touch off its owning thread (see
        // SDL3Window's thread-affinity contract).
        virtual void on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept = 0;

        // What this backend can actually do, populated during initialize().
        [[nodiscard]] virtual RendererCapabilities capabilities() const noexcept = 0;

        // Runtime threading envelope for this backend/API/platform combination. Vulkan permits host-side
        // multithreading, but many objects are externally synchronized: VkQueue, VkCommandPool, descriptor
        // pools/sets in some operations, and object destruction must not race uses. Backends should only
        // advertise parallel command recording once they provide per-thread command pools and ownership.
        [[nodiscard]] virtual RHI::RenderThreadingCapabilities render_threading_capabilities() const noexcept;

        // RHI escape hatch for high-level renderer systems that need API-agnostic low-level access.
        // Backends return nullptr until the RHI bridge is initialized or when no RHI bridge exists yet.
        [[nodiscard]] virtual RHI::RhiDevice *rhi_device() noexcept;
        [[nodiscard]] virtual const RHI::RhiDevice *rhi_device() const noexcept;

        // Returns the RHI presentation surface paired with a backend-owned render surface. The backend,
        // not the Renderer, owns translating a window-provider surface (SDL/GLFW/native) into the concrete
        // API object, so higher layers do not create duplicate platform surfaces from raw native handles.
        [[nodiscard]] virtual RendererExpected<RHI::SurfaceHandle> rhi_surface_for(RenderSurfaceHandle surface);

        // Backend-agnostic description of the GPU currently in use (name, vendor, driver version,
        // ...) as plain strings/integers — no graphics-API types leak out. Returns nullopt until a
        // physical device has been selected (i.e. before a successful initialize()).
        [[nodiscard]] virtual optional<GpuInfo> gpu_info() const = 0;

        // Block until all in-flight GPU work is complete. Called automatically by the destructor;
        // also available as an explicit sync point before resource reloads or controlled shutdown.
        virtual void wait_idle() noexcept = 0;

        template <typename Backend, typename... Args>
            requires derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static unique_ptr<Backend> create(Args &&...args) {
            return unique_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }

        template <typename Backend, typename... Args>
            requires derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static unique_ptr<EngineBackend> create_erased(Args &&...args) {
            return unique_ptr<EngineBackend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }

        template <typename Backend, typename... Args>
            requires derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static shared_ptr<Backend> create_shared(Args &&...args) {
            return shared_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }
    };

    // Constructs the Vulkan backend without pulling volk or Vulkan headers into the caller.
    // This is the API-selection switch point: a future build swaps in Metal or WebGPU here.
    [[nodiscard]] unique_ptr<EngineBackend> create_vulkan_backend();

} // namespace SFT::Core
