module;

#include <vector>

export module Sturdy.Core:VulkanBackend;

import :EngineBackend;
import :RendererError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;

using std::vector;

export namespace SFT::Core::Vulkan {

    // Vulkan renderer backend — implements the API-agnostic EngineBackend contract.
    //
    // Intentionally a skeleton: the window↔renderer binding, frame loop, resize plumbing, and
    // capability reporting are all wired up so the actual Vulkan work can be built out in the
    // TODO blocks without touching the Engine, Application, or Platform layers.
    //
    // Destructor contract: ~VulkanBackend() calls wait_idle() first, then releases all Vulkan
    // objects in reverse creation order (surfaces → swapchains → device → allocator → instance).
    class VulkanBackend final : public EngineBackend {
      public:
        ~VulkanBackend() override;

        RendererResult initialize(const RendererCreateInfo &init) override;
        RendererExpected<RenderSurfaceHandle> create_surface(const RenderSurfaceCreateInfo &init) override;
        RendererResult destroy_surface(RenderSurfaceHandle surface) override;
        void on_resize(RenderSurfaceHandle surface, Extent2D new_extent) noexcept override;
        [[nodiscard]] RendererCapabilities capabilities() const noexcept override;
        RendererResult render_frame(RenderSurfaceHandle surface, const FrameInput &frame) override;
        void wait_idle() noexcept override;

      private:
        friend class ::SFT::Core::EngineBackend;
        explicit VulkanBackend(ConstructorKey key);

        struct SurfaceState {
            RenderSurfaceDescriptor descriptor{};
            Extent2D extent{};
            u32 frames_in_flight = 2;
            u32 generation = 0;
            bool active = false;
            bool swapchain_dirty = false;
        };

        [[nodiscard]] SurfaceState *surface_state(RenderSurfaceHandle handle) noexcept;
        [[nodiscard]] const SurfaceState *surface_state(RenderSurfaceHandle handle) const noexcept;
        [[nodiscard]] RenderSurfaceHandle allocate_surface_slot(const RenderSurfaceCreateInfo &init);

        RendererCreateInfo create_info_{};
        RendererCapabilities capabilities_{};
        vector<SurfaceState> surfaces_;
        bool initialized_ = false;

        // TODO(renderer): Vulkan objects, added in creation order so the destructor can
        // tear them down in reverse:
        //   GraphicsDevice   device_;    // instance, physical/logical device, queues, VMA allocator
        //   vector<SurfaceResources>;    // VkSurfaceKHR, swapchain, per-frame sync per surface
        //   RenderGraph      graph_;     // pipeline cache, descriptor pool, shared render state
    };

} // namespace SFT::Core::Vulkan
