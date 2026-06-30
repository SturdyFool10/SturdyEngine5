module;

#include <concepts>
#include <memory>
#include <utility>

export module Sturdy.Core:EngineBackend;

import Sturdy.Foundation;
import Sturdy.Platform;
import :RendererError;
import :Renderer;
import :RenderSurface;

using std::derived_from;
using std::shared_ptr;
using std::unique_ptr;

export namespace SFT::Core {

    // The renderer abstraction seam. Every graphics API (Vulkan today; Metal, WebGPU later) derives
    // from this. The Engine layer speaks only this interface and the API-agnostic types in
    // Renderer.cppm / RenderSurface.cppm — no Vulkan symbols ever leak upward.
    //
    // The backend owns ALL GPU state: instance, device, queues, allocator, surfaces, swapchains,
    // frame-pacing sync objects, the render graph, and its own worker threads. The Engine does not
    // construct or destroy surfaces, and it does not drive individual acquire/record/submit/present
    // steps — it hands work to render_frame() and lets the backend schedule it according to what its
    // API supports.
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
        //   2. Destroy all surfaces (swapchains, images, sync objects) in reverse creation order.
        //   3. Destroy the device, allocator, debug layer, and instance.
        // There is no separate shutdown() call — the destructor is the only teardown path.
        virtual ~EngineBackend() = default;

        EngineBackend(const EngineBackend &) = delete;
        EngineBackend &operator=(const EngineBackend &) = delete;
        EngineBackend(EngineBackend &&) = delete;
        EngineBackend &operator=(EngineBackend &&) = delete;

        // Bring up backend-global GPU state and construct the initial backend-owned present surface
        // for RendererCreateInfo::window. The returned handle is an opaque address for render calls
        // and resize-needed notifications only; surface construction/destruction stay backend-private.
        virtual RendererExpected<RenderSurfaceHandle> initialize(const RendererCreateInfo &init) = 0;

        // Adds another window's surface (and whatever per-window resources the backend needs —
        // swapchain, sync objects, ...) to an already-initialized backend, sharing the existing
        // device/queue/allocator. Every window's resources are stored backend-side, keyed by the
        // window's stable WindowId; this is the entry point for every window after the first.
        virtual RendererExpected<RenderSurfaceHandle> create_window_surface(
            Platform::Windowing::Window &window,
            u32 desired_frames_in_flight = 2) = 0;

        // Destroys one window's backend-owned resources (surface, swapchain, sync objects).
        // Safe to call for the primary surface returned by initialize() as well as any surface
        // returned by create_window_surface().
        virtual void destroy_window_surface(RenderSurfaceHandle surface) noexcept = 0;

        // Notify the backend that a surface needs resize handling. The backend owns the surface and
        // is responsible for querying the latest framebuffer extent, marking/rebuilding swapchains,
        // and handling resize-to-zero (minimized) without external surface mutation.
        virtual void on_surface_resize_needed(RenderSurfaceHandle surface) noexcept = 0;

        // What this backend can actually do, populated during initialize().
        [[nodiscard]] virtual RendererCapabilities capabilities() const noexcept = 0;

        // Execute one frame: acquire → record → submit → present.
        // The backend owns all scheduling decisions — which resources to record into,
        // how many frames to keep in flight, when to rebuild a dirty swapchain, and whether
        // to use async compute or multi-threaded recording. Returns an error only when the
        // backend cannot recover from surface/device loss internally.
        virtual RendererResult render_frame(RenderSurfaceHandle surface, const FrameInput &frame) = 0;

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
        static shared_ptr<Backend> create_shared(Args &&...args) {
            return shared_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }
    };

    // Constructs the Vulkan backend without pulling volk or Vulkan headers into the caller.
    // This is the API-selection switch point: a future build swaps in Metal or WebGPU here.
    [[nodiscard]] unique_ptr<EngineBackend> create_vulkan_backend();

} // namespace SFT::Core
