module;
#pragma region Imports
#include <optional>
#include <unordered_map>
#include <vulkan/vulkan_core.h>
#pragma endregion

export module Sturdy.Core:VulkanBackend;

#pragma region Imports
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
#pragma endregion

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
        [[nodiscard]] optional<GpuInfo> gpu_info() const override;
        RendererResult render_frame(RenderSurfaceHandle surface, const FrameInput &frame) override;
        void wait_idle() noexcept override;
        // end EngineBackend Compliance Functions

        // Look up a compiled shader module by the source file it came from plus its entry point name,
        // e.g. find_shader_module("Shaders/triangle", "vertexMain"). The source file is keyed without
        // its ".slang" extension, but a trailing ".slang" is accepted and stripped. Returns nullptr
        // if no such module was produced during createShaders().
        [[nodiscard]] const VulkanShaderModule *find_shader_module(const ustr &source_file,
                                                                   const ustr &entry_point) const noexcept;
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
        // Builds the per-surface frame-pacing resources (timeline semaphore + one command
        // pool/buffer/image-acquired-semaphore per frame in flight) and installs them on the
        // surface. Run once per window — for the primary during initVulkan(), and for every
        // window added later via create_window_surface().
        RendererResult createSurfaceFrameResources(VulkanSurface &surface);

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
        // the logical device, same as the shader modules they're built alongside. Per-frame sync
        // and command resources are no longer global — each VulkanSurface owns its own set (see
        // FrameResources in :VulkanSurface), so windows render fully independently.
        VulkanPipeline graphicsPipeline;
        VulkanPipelineLayout pipelinelayout;
    };

} // namespace SFT::Core::Vulkan
