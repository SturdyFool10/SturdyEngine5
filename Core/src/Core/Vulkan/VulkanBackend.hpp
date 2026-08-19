#pragma once

#include <Foundation/Foundation.hpp>
#include <RHI/Threading.hpp>
#pragma region Imports
#include <memory>
#include <optional>
#include <unordered_map>
#include <vulkan/vulkan_core.h>
#pragma endregion

#include <Core/EngineBackend.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <Core/RenderSurface.hpp>
#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/VulkanSurface.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <WindowManager/WindowManager.hpp>
#include <RHI/RHI.hpp>

using SFT::WindowManager::Window;
using SFT::WindowManager::WindowId;
using std::optional;
using std::unordered_map;

namespace SFT::Core::Vulkan {


    class VulkanBackend final : public EngineBackend {
      public:
        /// Destroys the `VulkanBackend` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~VulkanBackend() override;

        /// Initializes the `VulkanBackend` for use.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererExpected<RenderSurfaceHandle> initialize(const RendererCreateInfo &init) override;
        /// Creates a window surface from the supplied parameters.
        ///
        /// @param window Window used or affected by the operation.
        /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererExpected<RenderSurfaceHandle> create_window_surface(Window &window, u32 desired_frames_in_flight = 2) override;
        /// Destroys the window surface identified by the supplied parameters.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroy_window_surface(RenderSurfaceHandle surface) noexcept override;
        /// Handles the surface resize needed event.
        ///
        /// @param surface Surface used or affected by the operation.
        /// @param extent `extent` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept override;
        /// Returns the current or globally available capabilities value.
        ///
        /// @return Returns the current capabilities value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RendererCapabilities capabilities() const noexcept override;
        /// Returns the current render threading capabilities.
        ///
        /// @return Returns the current render threading capabilities value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::RenderThreadingCapabilities render_threading_capabilities() const noexcept override;
        /// Returns the current or globally available RHI device value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] RHI::RhiDevice *rhi_device() noexcept override;
        /// Returns the current or globally available RHI device value.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const RHI::RhiDevice *rhi_device() const noexcept override;
        /// Resolves the RHI surface associated with the supplied key, handle, or resource.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RendererExpected<RHI::SurfaceHandle> rhi_surface_for(RenderSurfaceHandle surface) override;
        /// Returns the current GPU info.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        [[nodiscard]] optional<GpuInfo> gpu_info() const override;
        /// Waits for idle to complete.
        ///
        /// @note This function does not throw exceptions.
        void wait_idle() noexcept override;


        /// Performs the init vulkan operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererExpected<RenderSurfaceHandle> initVulkan(const RendererCreateInfo &init);
        /// Performs the create vulkan instance operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererResult createVulkanInstance(const RendererCreateInfo &init);


        /// Finds the requested entry in the available state.
        ///
        /// @param init `init` value used by the operation.
        /// @param primary_surface Surface used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererResult findPhysicalDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        /// Performs the discover graphics queue operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param init `init` value used by the operation.
        /// @param primary_surface Surface used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererResult discoverGraphicsQueue(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        /// Performs the create device operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param init `init` value used by the operation.
        /// @param primary_surface Surface used or affected by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererResult createDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface);
        /// Performs the initialize vma operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        RendererResult initializeVMA(const RendererCreateInfo &init);
        /// Performs the install RHI bridge operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        void installRhiBridge();
        /// Returns the current or globally available HDR swapchain colorspace enabled value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool hdr_swapchain_colorspace_enabled() const noexcept;
        /// Returns the current or globally available HDR metadata enabled value.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool hdr_metadata_enabled() const noexcept;

      private:
        friend class ::SFT::Core::EngineBackend;
        /// Constructs a `VulkanBackend` from the supplied initialization values.
        ///
        /// @param key Key used to identify the requested entry.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit VulkanBackend(ConstructorKey key);

        struct SurfaceCreateInfo {
            Window *window = nullptr;
            RenderSurfaceDescriptor descriptor{};
            Extent2D framebuffer_extent{};
            u32 desired_frames_in_flight = 2;
        };

        /// Performs the destroy vulkan resources operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @note This function does not throw exceptions.
        void destroyVulkanResources() noexcept;
        /// Performs the surface create info from window operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param window Window used or affected by the operation.
        /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RendererExpected<SurfaceCreateInfo> surface_create_info_from_window(Window &window,
                                                                                          u32 desired_frames_in_flight) const;
        /// Performs the create surface operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param init `init` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] RendererExpected<RenderSurfaceHandle> createSurface(const SurfaceCreateInfo &init);
        /// Performs the destroy surface operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param surface Surface used or affected by the operation.
        ///
        /// @note This function does not throw exceptions.
        void destroySurface(VulkanSurface &surface) noexcept;
        /// Destroys the all surfaces identified by the supplied parameters.
        ///
        /// @note This function does not throw exceptions.
        void destroy_all_surfaces() noexcept;
        /// Performs the surface slot operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VulkanSurface *surface_slot(RenderSurfaceHandle handle) noexcept;
        /// Performs the surface slot operation for `VulkanBackend` using the supplied arguments.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const VulkanSurface *surface_slot(RenderSurfaceHandle handle) const noexcept;

        RendererCreateInfo create_info_{};
        RendererCapabilities capabilities_{};


        unordered_map<WindowId, VulkanSurface> surfaces_;

        bool initialized_ = false;

        VkInstance vulkan_instance = VK_NULL_HANDLE;
        VulkanPhysicalDevice physicalDevice;
        VulkanDevice logicalDevice;
        VulkanQueue gfxQueue;
        VulkanAllocator vmaAllocator;
        RHI::FeatureNegotiationReport feature_report_{};
        bool hdr_swapchain_colorspace_enabled_ = false;
        bool hdr_metadata_enabled_ = false;


        bool surface_capabilities2_enabled_ = false;
        bool surface_maintenance1_enabled_ = false;
        std::unique_ptr<RHI::RhiDevice> rhiDevice;
    };

} // namespace SFT::Core::Vulkan
