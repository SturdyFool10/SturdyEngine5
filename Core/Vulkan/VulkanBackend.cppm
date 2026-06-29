module;
#include <vector>
#include <vulkan/vulkan_core.h>

export module Sturdy.Core:VulkanBackend;

import :EngineBackend;
import :RendererError;
import :Renderer;
import :RenderSurface;
import :VulkanSurface;
import :VulkanPhysicalDevice;
import Sturdy.Foundation;
import Sturdy.Platform;

using std::vector;
using SFT::Platform::Windowing::Window;

export namespace SFT::Core::Vulkan {

    // Vulkan renderer backend — implements the API-agnostic EngineBackend contract.
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
        RendererResult createDevice(const RendererCreateInfo &init);
        RendererResult initializeVMA(const RendererCreateInfo &init);

      private:
        friend class ::SFT::Core::EngineBackend;
        explicit VulkanBackend(ConstructorKey key);

        struct SurfaceCreateInfo {
            Window *window = nullptr;
            RenderSurfaceDescriptor descriptor{};
            Extent2D framebuffer_extent{};
            u32 desired_frames_in_flight = 2;
        };

        [[nodiscard]] RendererExpected<SurfaceCreateInfo> surface_create_info_from_window(Window &window,
                                                                                          u32 desired_frames_in_flight) const;
        [[nodiscard]] RendererExpected<RenderSurfaceHandle> createSurface(const SurfaceCreateInfo &init);
        void destroy_all_surfaces() noexcept;
        [[nodiscard]] VulkanSurface *surface_slot(RenderSurfaceHandle handle) noexcept;
        [[nodiscard]] const VulkanSurface *surface_slot(RenderSurfaceHandle handle) const noexcept;

        RendererCreateInfo create_info_{};
        RendererCapabilities capabilities_{};
        vector<VulkanSurface> surfaces_;
        bool initialized_ = false;

        // Non-owning view of the primary window, supplied via RendererCreateInfo::window at
        // initialize() time. The window is owned by the application and outlives the backend.
        Window *window_ = nullptr;

        // TODO(renderer): Vulkan objects, added in creation order so the destructor can
        // tear them down in reverse:
        //   GraphicsDevice   device_;    // logical device, queues, VMA allocator
        //   vector<SurfaceResources>;    // swapchain, per-frame sync per surface
        //   RenderGraph      graph_;     // pipeline cache, descriptor pool, shared render state
        VkInstance vulkan_instance = VK_NULL_HANDLE;
        VulkanPhysicalDevice physical_device_;
    };

} // namespace SFT::Core::Vulkan
