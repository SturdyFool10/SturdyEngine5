#pragma once

#include "Core/EngineBackend.hpp"

#include <vector>

using std::vector;

namespace SFT::Core::Vulkan {

    // Vulkan renderer backend - implements the API-agnostic EngineBackend contract.
    //
    // This is intentionally a thin skeleton. The window<->renderer binding, frame loop, resize
    // plumbing and capability reporting are all wired up around it, so the actual Vulkan
    // renderer can be built out from here without touching the Engine/Application/Platform
    // layers. Everything window-specific arrives through `create_surface()` / `recreate_surface()`,
    // and frames target a RenderSurfaceHandle; nothing above this class knows or cares that it is
    // Vulkan.
    class VulkanBackend final : public EngineBackend {
      public:
        ~VulkanBackend() override;

        RendererResult initialize(const RendererCreateInfo &init) override;
        RendererExpected<RenderSurfaceHandle> create_surface(const RenderSurfaceCreateInfo &init) override;
        RendererResult destroy_surface(RenderSurfaceHandle surface) override;
        RendererResult resize_surface(RenderSurfaceHandle surface, Extent2D extent) override;
        RendererResult recreate_surface(RenderSurfaceHandle surface, const RenderSurfaceCreateInfo &init) override;
        [[nodiscard]] RendererCapabilities capabilities() const override;
        RendererResult render_frame(RenderSurfaceHandle surface, const FrameInput &frame) override;
        void wait_idle(RenderSurfaceHandle surface) noexcept override;
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

        [[nodiscard]] SurfaceState *surface_state(RenderSurfaceHandle surface) noexcept;
        [[nodiscard]] const SurfaceState *surface_state(RenderSurfaceHandle surface) const noexcept;
        [[nodiscard]] RenderSurfaceHandle allocate_surface_slot(const RenderSurfaceCreateInfo &init);

        RendererCreateInfo create_info_{};
        RendererCapabilities capabilities_{};
        vector<SurfaceState> surfaces_;
        bool initialized_ = false;

        // TODO(renderer): own the Vulkan objects here as they are built out, e.g.
        //   GraphicsDevice device_;   // instance, physical/logical device, queues, VMA allocator
        //   vector<SurfaceResources>; // VkSurfaceKHR, swapchain, per-surface frames
        //   RenderGraph    graph_;         // shared graph/pipeline/cache state
    };

} // namespace SFT::Core::Vulkan
