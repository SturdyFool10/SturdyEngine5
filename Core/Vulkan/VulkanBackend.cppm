module;
#pragma region Imports
#include <memory>
#include <optional>
#include <unordered_map>
#include <vulkan/vulkan_core.h>
#pragma endregion

export module Sturdy.Core:VulkanBackend;

import :EngineBackend;
import :GraphicsBackendError;
import :Renderer;
import :RenderSurface;
import :VulkanAllocator;
import :VulkanDevice;
import :VulkanQueue;
import :VulkanSurface;
import :VulkanPhysicalDevice;
import Sturdy.Foundation;
import Sturdy.Platform;
import Sturdy.RHI;

using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowId;
using std::optional;
using std::unordered_map;

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
        RendererExpected<RenderSurfaceHandle> create_window_surface(Window &window, u32 desired_frames_in_flight = 2) override;
        void destroy_window_surface(RenderSurfaceHandle surface) noexcept override;
        void on_surface_resize_needed(RenderSurfaceHandle surface) noexcept override;
        [[nodiscard]] RendererCapabilities capabilities() const noexcept override;
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept override;
        [[nodiscard]] const RHI::RhiDevice *rhi_device() const noexcept override;
        [[nodiscard]] optional<GpuInfo> gpu_info() const override;
        void wait_idle() noexcept override;
        // end EngineBackend Compliance Functions

        RendererExpected<RenderSurfaceHandle> initVulkan(const RendererCreateInfo &init);
        RendererResult createVulkanInstance(const RendererCreateInfo &init);
        // GPU/device bring-up only ever runs once, against the surface of the first window
        // passed to initialize() — every subsequent window shares the resulting device/queue.
        RendererResult findPhysicalDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        RendererResult discoverGraphicsQueue(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        RendererResult createDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        RendererResult initializeVMA(const RendererCreateInfo &init);
        void installRhiBridge();

      private:
        friend class ::SFT::Core::EngineBackend;
        explicit VulkanBackend(ConstructorKey key);

        struct SurfaceCreateInfo {
            Window *window = nullptr;
            RenderSurfaceDescriptor descriptor{};
            Extent2D framebuffer_extent{};
            u32 desired_frames_in_flight = 2;
        };

        void destroyVulkanResources() noexcept;
        [[nodiscard]] RendererExpected<SurfaceCreateInfo> surface_create_info_from_window(Window &window,
                                                                                          u32 desired_frames_in_flight) const;
        [[nodiscard]] RendererExpected<RenderSurfaceHandle> createSurface(const SurfaceCreateInfo &init);
        void destroySurface(VulkanSurface &surface) noexcept;
        void destroy_all_surfaces() noexcept;
        [[nodiscard]] VulkanSurface *surface_slot(RenderSurfaceHandle handle) noexcept;
        [[nodiscard]] const VulkanSurface *surface_slot(RenderSurfaceHandle handle) const noexcept;

        RendererCreateInfo create_info_{};
        RendererCapabilities capabilities_{};

        // Every per-window GPU resource bundle (surface + swapchain, eventually per-frame sync
        // objects) lives here, keyed by the owning window's stable WindowId. Windows are never
        // recycled into a reused slot — when one is destroyed its entry is erased outright.
        unordered_map<WindowId, VulkanSurface> surfaces_;

        bool initialized_ = false;

        VkInstance vulkan_instance = VK_NULL_HANDLE;
        bool volk_initialized_ = false;
        VulkanPhysicalDevice physicalDevice;
        VulkanDevice logicalDevice;
        VulkanQueue gfxQueue;
        VulkanAllocator vmaAllocator;
        RHI::FeatureNegotiationReport feature_report_{};
        std::unique_ptr<RHI::RhiDevice> rhiDevice;
    };

} // namespace SFT::Core::Vulkan
