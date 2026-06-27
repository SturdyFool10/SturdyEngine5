module;

#include <concepts>
#include <memory>
#include <utility>

export module Sturdy.Core:EngineBackend;

import Sturdy.Foundation;
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
    // The backend owns ALL GPU state: instance, device, queues, allocator, swapchains, frame-pacing
    // sync objects, the render graph, and its own worker threads. The Engine does not drive
    // individual acquire/record/submit/present steps — it hands work to render_frame() and lets the
    // backend schedule it according to what its API supports.
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

        // Bring up backend-global GPU state: instance, physical device, logical device,
        // queues, and allocator. The surface provider/system in `init` is required here
        // because some APIs (Vulkan) need WSI extension names at instance creation time,
        // before any surface handle exists.
        virtual RendererResult initialize(const RendererCreateInfo &init) = 0;

        // Create one backend-owned present surface for a window. The backend allocates all
        // per-surface GPU resources (API surface handle, swapchain, per-frame sync objects)
        // and returns a stable generational handle. The window itself remains owned by Platform.
        virtual RendererExpected<RenderSurfaceHandle> create_surface(const RenderSurfaceCreateInfo &init) = 0;

        // Drain all in-flight GPU work for this surface, then release every GPU resource it owns.
        // After this returns the handle is invalid — do not pass it to any other method.
        // If the native window handle changed (surface loss, display migration), call
        // destroy_surface + create_surface rather than attempting to reuse the handle.
        virtual RendererResult destroy_surface(RenderSurfaceHandle surface) = 0;

        // Called whenever a surface's framebuffer extent changes, including resize-to-zero
        // (minimized window). The backend stores the new extent, marks the swapchain dirty,
        // and rebuilds lazily at the start of the next render_frame call. Passing zero extent
        // is valid — the backend simply skips presentation until the extent is non-zero again.
        virtual void on_resize(RenderSurfaceHandle surface, Extent2D new_extent) noexcept = 0;

        // What this backend can actually do, populated during initialize().
        [[nodiscard]] virtual RendererCapabilities capabilities() const noexcept = 0;

        // Execute one frame: acquire → record → submit → present.
        // The backend owns all scheduling decisions — which resources to record into,
        // how many frames to keep in flight, when to rebuild a dirty swapchain, and whether
        // to use async compute or multi-threaded recording. Returns an error only when the
        // surface is permanently lost and a new surface must be created by the caller.
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
