#pragma once

#include "Core/EngineBackend.hpp"

namespace SFT::Core::Vulkan {

    // Vulkan renderer backend — implements the API-agnostic EngineBackend contract.
    //
    // This is intentionally a thin skeleton. The window<->renderer binding, frame loop, resize
    // plumbing and capability reporting are all wired up around it, so the actual Vulkan
    // renderer can be built out from here without touching the Engine/Application/Platform
    // layers. Everything the backend receives from the outside arrives through `initialize()`
    // (surface + extent + requested features) and `render_frame()` (per-frame input); nothing
    // above this class knows or cares that it is Vulkan.
    class VulkanBackend final : public EngineBackend {
    public:
        ~VulkanBackend() override;

        RendererResult initialize(const RendererInit& init) override;
        [[nodiscard]] RendererCapabilities capabilities() const override;
        RendererResult render_frame(const FrameInput& frame) override;
        RendererResult on_resize(Extent2D extent) override;
        void wait_idle() noexcept override;

    private:
        friend class ::SFT::Core::EngineBackend;
        explicit VulkanBackend(ConstructorKey key);

        // Bound at initialize() — what the renderer draws into and how big it is.
        RenderSurfaceDescriptor surface_ {};
        Extent2D extent_ {};
        RendererCapabilities capabilities_ {};
        bool initialized_ = false;

        // TODO(renderer): own the Vulkan objects here as they are built out, e.g.
        //   GraphicsDevice device_;   // instance, physical/logical device, queues, VMA allocator
        //   Swapchain      swapchain_;
        //   FrameContext   frames_[capabilities_.max_frames_in_flight];
        //   RenderGraph    graph_;    // deferred passes; swap shadow->RT, AO, etc. as nodes
    };

} // namespace SFT::Core::Vulkan
