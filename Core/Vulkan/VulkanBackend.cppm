module;

#include <vector>
#include <vulkan/vulkan_core.h>

export module Sturdy.Core:VulkanBackend;

import :EngineBackend;
import :RendererError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;
import Sturdy.Platform;

using std::vector;
using SFT::Platform::Windowing::Window;

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
        // begin EngineBackend Compliance Functions
        RendererExpected<RenderSurfaceHandle> initialize(const RendererCreateInfo &init) override;
        void on_surface_resize_needed(RenderSurfaceHandle surface) noexcept override;
        [[nodiscard]] RendererCapabilities capabilities() const noexcept override;
        RendererResult render_frame(RenderSurfaceHandle surface, const FrameInput &frame) override;
        void wait_idle() noexcept override;
        // end EngineBackend Compliance Functions
        RendererExpected<RenderSurfaceHandle> initVulkan(const RendererCreateInfo &init);
        RendererResult createVulkanInstance(const RendererCreateInfo &init);
        RendererResult findPhysicalDevice(const RendererCreateInfo &init);
        RendererResult discoverGraphicsQueue(const RendererCreateInfo &init);

      private:
        friend class ::SFT::Core::EngineBackend;
        explicit VulkanBackend(ConstructorKey key);

        struct SurfaceCreateInfo {
            Window *window = nullptr;
            RenderSurfaceDescriptor descriptor{};
            Extent2D framebuffer_extent{};
            u32 desired_frames_in_flight = 2;
        };

        struct SurfaceState {
            Window *window = nullptr;
            RenderSurfaceDescriptor descriptor{};
            Extent2D extent{};
            u32 frames_in_flight = 2;
            u32 generation = 0;
            bool active = false;
            bool swapchain_dirty = false;
        };

        [[nodiscard]] RendererExpected<SurfaceCreateInfo> surface_create_info_from_window(Window &window,
                                                                                          u32 desired_frames_in_flight) const;
        [[nodiscard]] RendererExpected<RenderSurfaceHandle> createSurface(const SurfaceCreateInfo &init);
        void release_surface_state(SurfaceState &state) noexcept;
        void destroy_all_surfaces() noexcept;
        void refresh_surface_extent(SurfaceState &state) noexcept;
        [[nodiscard]] SurfaceState *surface_state(RenderSurfaceHandle handle) noexcept;
        [[nodiscard]] const SurfaceState *surface_state(RenderSurfaceHandle handle) const noexcept;
        [[nodiscard]] RenderSurfaceHandle allocate_surface_slot(const SurfaceCreateInfo &init);

        RendererCreateInfo create_info_{};
        RendererCapabilities capabilities_{};
        vector<SurfaceState> surfaces_;
        bool initialized_ = false;

        // Non-owning view of the primary window the renderer presents into, supplied via
        // RendererCreateInfo::window at initialize() time. The window is owned by the
        // application/engine layer and outlives the backend. Used for backend-owned surface
        // creation and framebuffer extent queries.
        Window *window_ = nullptr;

        // TODO(renderer): Vulkan objects, added in creation order so the destructor can
        // tear them down in reverse:
        //   GraphicsDevice   device_;    // instance, physical/logical device, queues, VMA allocator
        //   vector<SurfaceResources>;    // VkSurfaceKHR, swapchain, per-frame sync per surface
        //   RenderGraph      graph_;     // pipeline cache, descriptor pool, shared render state
        VkInstance vulkan_instance;
    };

} // namespace SFT::Core::Vulkan
