module;
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan_core.h>

export module Sturdy.Core:VulkanBackend;

import :EngineBackend;
import :RendererError;
import :Renderer;
import :RenderSurface;
import :VulkanAllocator;
import :VulkanCommandBuffer;
import :VulkanCommandPool;
import :VulkanDevice;
import :VulkanQueue;
import :VulkanShaderModule;
import :VulkanSurface;
import :VulkanSwapchain;
import :VulkanPhysicalDevice;
import :VulkanPipeline;
import :VulkanSync;
import Sturdy.Foundation;
import Sturdy.Platform;

using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowId;
using std::string_view;
using std::unordered_map;
using std::vector;

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
        RendererResult render_frame(RenderSurfaceHandle surface, const FrameInput &frame) override;
        void wait_idle() noexcept override;
        // end EngineBackend Compliance Functions

        // Look up a compiled shader module by the source file it came from plus its entry point name,
        // e.g. find_shader_module("Shaders/triangle", "vertexMain"). The source file is keyed without
        // its ".slang" extension, but a trailing ".slang" is accepted and stripped. Returns nullptr
        // if no such module was produced during createShaders().
        [[nodiscard]] const VulkanShaderModule *find_shader_module(string_view source_file,
                                                                   string_view entry_point) const noexcept;
        RendererExpected<RenderSurfaceHandle> initVulkan(const RendererCreateInfo &init);
        RendererResult createVulkanInstance(const RendererCreateInfo &init);
        // GPU/device bring-up only ever runs once, against the surface of the first window
        // passed to initialize() — every subsequent window shares the resulting device/queue.
        RendererResult findPhysicalDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        RendererResult discoverGraphicsQueue(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        RendererResult createDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        RendererResult initializeVMA(const RendererCreateInfo &init);
        RendererResult createShaders(const RendererCreateInfo &init);
        RendererResult createGraphicsPipeline(const RendererCreateInfo &init);
        RendererResult createSyncResources(const RendererCreateInfo &init);
        RendererResult createCommandBuffers(const RendererCreateInfo &init);

      private:
        friend class ::SFT::Core::EngineBackend;
        explicit VulkanBackend(ConstructorKey key);

        struct SurfaceCreateInfo {
            Window *window = nullptr;
            RenderSurfaceDescriptor descriptor{};
            Extent2D framebuffer_extent{};
            u32 desired_frames_in_flight = 2;
        };

        struct FrameResources {
            VulkanSemaphore imageAcquiredSemaphore;
            VulkanCommandPool commandPool;
            VulkanCommandBuffer commandBuffer;

            void destroyCommandResources() noexcept {
                commandBuffer.destroy();
                commandPool.destroy();
            }

            void destroy() noexcept {
                destroyCommandResources();
                imageAcquiredSemaphore.destroy();
            }
        };

        void destroyFrameResources() noexcept;
        void destroyVulkanResources() noexcept;
        [[nodiscard]] RendererExpected<SurfaceCreateInfo> surface_create_info_from_window(Window &window,
                                                                                          u32 desired_frames_in_flight) const;
        [[nodiscard]] RendererExpected<RenderSurfaceHandle> createSurface(const SurfaceCreateInfo &init);
        RendererResult createSwapchain(const RendererCreateInfo &init, VulkanSurface &surface);
        void destroySwapchain(VulkanSurface &surface) noexcept;
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

        // Every SPIR-V shader module compiled from the engine's UnCompiledShader list during
        // initialize(), keyed by (source file, entry point). Each module retains a shared handle to
        // its source file's reflection. Destroyed in ~VulkanBackend before the logical device.
        unordered_map<VulkanShaderModuleKey, VulkanShaderModule, VulkanShaderModuleKeyHash> shader_modules_;
        bool initialized_ = false;

        VkInstance vulkan_instance = VK_NULL_HANDLE;
        bool volk_initialized_ = false;
        VulkanPhysicalDevice physicalDevice;
        VulkanDevice logicalDevice;
        VulkanQueue gfxQueue;
        VulkanAllocator vmaAllocator;
        // Device-owned resources built during initialization. Destroyed in ~VulkanBackend before
        // the logical device, same as the shader modules they're built alongside.
        vector<FrameResources> frameResources_;
        VulkanSemaphore frameTimelineSemaphore;
        VulkanPipeline graphicsPipeline;
        VkPipelineLayout pipelinelayout = VK_NULL_HANDLE;
        u32 FrameIndex = 0;
        u32 MaxFramesInFlight = 2;
        u64 nextSignalValue = MaxFramesInFlight + 1;
    };

} // namespace SFT::Core::Vulkan
