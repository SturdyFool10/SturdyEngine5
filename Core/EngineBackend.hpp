#pragma once

#include "Core/Renderer.hpp"

#include <concepts>
#include <memory>
#include <utility>

namespace SFT::Core {

    // The renderer abstraction seam. Every graphics API (Vulkan today; Metal, WebGPU later) is a
    // subclass. The glue (Engine) speaks only this interface and the API-agnostic types in
    // Renderer.hpp / RenderSurface.hpp - no Vulkan ever leaks upward. The backend owns ALL
    // graphics state: device, swapchain, frame pacing, the render graph, and its own threading.
    class EngineBackend {
      protected:
        struct ConstructorKey {
          private:
            friend class EngineBackend;
            constexpr ConstructorKey() = default;
        };

        explicit constexpr EngineBackend(ConstructorKey) noexcept {}

      public:
        virtual ~EngineBackend() = default;

        EngineBackend(const EngineBackend &) = delete;
        EngineBackend &operator=(const EngineBackend &) = delete;
        EngineBackend(EngineBackend &&) = delete;
        EngineBackend &operator=(EngineBackend &&) = delete;

        // --- Renderer interface (API-agnostic) -----------------------------------------
        // Bring up backend-global graphics state: instance/device/queues/allocator. A backend may
        // use the initial surface provider/system to enable the right window-system extensions.
        virtual RendererResult initialize(const RendererCreateInfo &init) = 0;
        // Create/destroy one backend-owned present surface. Each surface owns its own API surface,
        // swapchain and swapchain-dependent resources; the window remains owned by Platform.
        virtual RendererExpected<RenderSurfaceHandle> create_surface(const RenderSurfaceCreateInfo &init) = 0;
        virtual RendererResult destroy_surface(RenderSurfaceHandle surface) = 0;
        // Resize only recreates swapchain-dependent resources. Recreate is for a changed/lost
        // native window surface and may rebuild the API surface itself while keeping the handle.
        virtual RendererResult resize_surface(RenderSurfaceHandle surface, Extent2D extent) = 0;
        virtual RendererResult recreate_surface(RenderSurfaceHandle surface, const RenderSurfaceCreateInfo &init) = 0;
        // What this backend can actually do (queried after initialize()).
        [[nodiscard]] virtual RendererCapabilities capabilities() const = 0;
        // Render and present one frame to the selected surface. The backend owns acquire/record/submit/present and
        // (eventually) its own worker threads and render-graph passes.
        virtual RendererResult render_frame(RenderSurfaceHandle surface, const FrameInput &frame) = 0;
        // Block until the device is idle (safe teardown / pre-resize).
        virtual void wait_idle(RenderSurfaceHandle surface) noexcept = 0;
        virtual void wait_idle() noexcept = 0;
        // --------------------------------------------------------------------------------

        template <typename Backend, typename... Args>
            requires std::derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static std::unique_ptr<Backend> create(Args &&...args) {
            return std::unique_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }

        template <typename Backend, typename... Args>
            requires std::derived_from<Backend, EngineBackend> && requires(Args &&...args) {
                new Backend(ConstructorKey{}, std::forward<Args>(args)...);
            }
        [[nodiscard]]
        static std::shared_ptr<Backend> create_shared(Args &&...args) {
            return std::shared_ptr<Backend>(new Backend(ConstructorKey{}, std::forward<Args>(args)...));
        }
    };

    // Constructs the Vulkan backend without dragging volk/Vulkan headers into the caller. This
    // is the API-selection switch point: a future build picks Vulkan/Metal/WebGPU here.
    [[nodiscard]] std::unique_ptr<EngineBackend> create_vulkan_backend();

} // namespace SFT::Core
