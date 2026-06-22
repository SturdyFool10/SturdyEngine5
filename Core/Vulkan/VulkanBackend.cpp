#include "Core/Vulkan/VulkanBackend.hpp"

namespace SFT::Core::Vulkan {

    VulkanBackend::VulkanBackend(ConstructorKey key)
        : EngineBackend(key) {
    }

    VulkanBackend::~VulkanBackend() {
        wait_idle();
        // TODO(renderer): tear down Vulkan objects in reverse creation order.
    }

    RendererResult VulkanBackend::initialize() {
        // Advertise the feature set a Vulkan backend is expected to provide. Replace these
        // hard-coded values with real VkPhysicalDevice queries once device selection exists.
        // max_frames_in_flight is finalized in bind_surface(), once the swapchain (and the
        // engine's desired_frames_in_flight request) is known.
        capabilities_ = RendererCapabilities{};
        capabilities_.multithreaded_command_recording = true;
        capabilities_.timeline_semaphores = true;

        // TODO(renderer): bring up the window-independent Vulkan core here. A reasonable order:
        //   1. volk init — reuse the window's loader:
        //      volkInitializeCustom((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr());
        //   2. VkInstance (+ debug messenger in Debug); instance extensions from
        //      SDL_Vulkan_GetInstanceExtensions(&count); then volkLoadInstance(instance).
        //   3. Select a Vulkan 1.4 physical device; create the logical device (dynamic
        //      rendering, synchronization2, timeline semaphores) + VMA allocator;
        //      volkLoadDevice(device). Present-queue support is verified later in
        //      bind_surface(), once a VkSurfaceKHR exists.

        initialized_ = true;
        return {};
    }

    RendererResult VulkanBackend::bind_surface(const RendererInit &init) {
        surface_ = init.surface;
        extent_ = init.framebuffer_extent;
        capabilities_.max_frames_in_flight =
            init.features.desired_frames_in_flight == 0 ? 2u : init.features.desired_frames_in_flight;

        // TODO(renderer): now that a window exists, complete surface-dependent bring-up:
        //   1. VkSurfaceKHR via SDL_Vulkan_CreateSurface(surface_.sdl_window, instance, nullptr, &surface);
        //   2. Verify the chosen device/queue can present to this surface.
        //   3. Create the swapchain from extent_ and per-frame command pools / sync objects.

        surface_bound_ = true;
        return {};
    }

    RendererCapabilities VulkanBackend::capabilities() const {
        return capabilities_;
    }

    RendererResult VulkanBackend::render_frame(const FrameInput &frame) {
        (void)frame;
        if (!initialized_ || !surface_bound_ || extent_.is_zero()) {
            return {};
        }

        // TODO(renderer): acquire -> record (Vulkan 1.4 dynamic rendering) -> submit
        // (synchronization2 / vkQueueSubmit2) -> present. Recreate the swapchain on
        // VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR.
        return {};
    }

    RendererResult VulkanBackend::on_resize(Extent2D extent) {
        extent_ = extent;
        // TODO(renderer): wait_idle() + recreate the swapchain when extent is non-zero.
        return {};
    }

    void VulkanBackend::wait_idle() noexcept {
        // TODO(renderer): vkDeviceWaitIdle(device) once a device exists.
    }

} // namespace SFT::Core::Vulkan

namespace SFT::Core {

    std::unique_ptr<EngineBackend> create_vulkan_backend() {
        return EngineBackend::create<Vulkan::VulkanBackend>();
    }

} // namespace SFT::Core
