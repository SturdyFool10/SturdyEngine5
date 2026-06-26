#include "Core/Vulkan/VulkanBackend.hpp"

#include <new>

using std::bad_alloc;
using std::unexpected;
using std::unique_ptr;

namespace SFT::Core::Vulkan {

    namespace {

        [[nodiscard]] u32 sanitize_frames_in_flight(u32 requested) noexcept {
            return requested == 0 ? 2u : requested;
        }

        [[nodiscard]] u32 next_generation(u32 current) noexcept {
            ++current;
            return current == 0 ? 1u : current;
        }

    } // namespace

    VulkanBackend::VulkanBackend(ConstructorKey key)
        : EngineBackend(key) {
    }

    VulkanBackend::~VulkanBackend() {
        wait_idle();
        // TODO(renderer): tear down Vulkan objects in reverse creation order.
    }

    RendererResult VulkanBackend::initialize(const RendererCreateInfo &init) {
        create_info_ = init;

        // Advertise the feature set a Vulkan backend is expected to provide. Replace these
        // hard-coded values with real VkPhysicalDevice queries once device selection exists.
        capabilities_ = RendererCapabilities{};
        capabilities_.multithreaded_command_recording = true;
        capabilities_.timeline_semaphores = true;
        capabilities_.max_frames_in_flight = sanitize_frames_in_flight(init.features.desired_frames_in_flight);

        // TODO(renderer): bring up the backend-global Vulkan core here. A reasonable order:
        //   1. volk init. If using SDL/GLFW helpers, use the provider from create_info_ to get
        //      the loader proc address and required WSI instance extensions.
        //   2. VkInstance (+ debug messenger in Debug), enabling WSI extensions for
        //      create_info_.initial_surface_provider / create_info_.initial_surface_system.
        //   3. Select a Vulkan 1.4 physical device; create the logical device (dynamic
        //      rendering, synchronization2, timeline semaphores) + VMA allocator;
        //      volkLoadDevice(device). Present-queue support is verified per surface.

        initialized_ = true;
        return {};
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::create_surface(const RenderSurfaceCreateInfo &init) {
        if (!initialized_) {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed, "Vulkan backend must be initialized before creating a surface."});
        }

        try {
            RenderSurfaceHandle handle = allocate_surface_slot(init);

            // TODO(renderer): create per-window resources here:
            //   1. VkSurfaceKHR from init.descriptor.provider/provider_window or native handles.
            //   2. Verify the chosen graphics/present queues can present to this surface.
            //   3. Create the swapchain and per-surface frame resources when extent is non-zero.
            return handle;
        } catch (const bad_alloc &) {
            return unexpected(RendererError{RendererErrorCode::OutOfMemory, "Out of memory while tracking a Vulkan render surface."});
        }
    }

    RendererResult VulkanBackend::destroy_surface(RenderSurfaceHandle surface) {
        SurfaceState *state = surface_state(surface);
        if (!state) {
            return renderer_error(RendererErrorCode::SurfaceLost, "Invalid Vulkan render surface handle.");
        }

        wait_idle(surface);

        // TODO(renderer): destroy this surface's swapchain-dependent resources, then VkSurfaceKHR.
        const u32 generation = state->generation;
        *state = SurfaceState{};
        state->generation = generation;
        return {};
    }

    RendererResult VulkanBackend::resize_surface(RenderSurfaceHandle surface, Extent2D extent) {
        SurfaceState *state = surface_state(surface);
        if (!state) {
            return renderer_error(RendererErrorCode::SurfaceLost, "Invalid Vulkan render surface handle.");
        }

        if (state->extent.width != extent.width || state->extent.height != extent.height) {
            state->extent = extent;
            state->swapchain_dirty = true;
        }

        // TODO(renderer): keep this cheap. Recreate the swapchain lazily on the next render for
        // this surface, and pause presentation while extent is zero.
        return {};
    }

    RendererResult VulkanBackend::recreate_surface(RenderSurfaceHandle surface, const RenderSurfaceCreateInfo &init) {
        SurfaceState *state = surface_state(surface);
        if (!state) {
            return renderer_error(RendererErrorCode::SurfaceLost, "Invalid Vulkan render surface handle.");
        }

        wait_idle(surface);

        state->descriptor = init.descriptor;
        state->extent = init.framebuffer_extent;
        state->frames_in_flight = sanitize_frames_in_flight(init.desired_frames_in_flight);
        state->swapchain_dirty = true;

        // TODO(renderer): destroy the old swapchain and VkSurfaceKHR, create a new VkSurfaceKHR
        // from init.descriptor, then rebuild swapchain resources when extent is non-zero.
        return {};
    }

    RendererCapabilities VulkanBackend::capabilities() const {
        return capabilities_;
    }

    RendererResult VulkanBackend::render_frame(RenderSurfaceHandle surface, const FrameInput &frame) {
        (void)frame;
        SurfaceState *state = surface_state(surface);
        if (!state) {
            return renderer_error(RendererErrorCode::SurfaceLost, "Invalid Vulkan render surface handle.");
        }

        if (state->extent.is_zero()) {
            return {};
        }

        if (state->swapchain_dirty) {
            // TODO(renderer): wait for this surface's in-flight work, recreate the swapchain and
            // swapchain-sized attachments, then clear the dirty flag only after success.
            state->swapchain_dirty = false;
        }

        // TODO(renderer): acquire -> record (Vulkan 1.4 dynamic rendering) -> submit
        // (synchronization2 / vkQueueSubmit2) -> present. Recreate the swapchain on
        // VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR.
        return {};
    }

    void VulkanBackend::wait_idle(RenderSurfaceHandle surface) noexcept {
        if (!surface_state(surface)) {
            return;
        }

        // TODO(renderer): wait only for work associated with this surface when possible. Early
        // implementation can fall back to vkDeviceWaitIdle(device).
    }

    void VulkanBackend::wait_idle() noexcept {
        // TODO(renderer): vkDeviceWaitIdle(device) once a device exists.
    }

    VulkanBackend::SurfaceState *VulkanBackend::surface_state(RenderSurfaceHandle surface) noexcept {
        if (!surface.is_valid() || static_cast<usize>(surface.index) >= surfaces_.size()) {
            return nullptr;
        }

        SurfaceState &state = surfaces_[static_cast<usize>(surface.index)];
        if (!state.active || state.generation != surface.generation) {
            return nullptr;
        }

        return &state;
    }

    const VulkanBackend::SurfaceState *VulkanBackend::surface_state(RenderSurfaceHandle surface) const noexcept {
        if (!surface.is_valid() || static_cast<usize>(surface.index) >= surfaces_.size()) {
            return nullptr;
        }

        const SurfaceState &state = surfaces_[static_cast<usize>(surface.index)];
        if (!state.active || state.generation != surface.generation) {
            return nullptr;
        }

        return &state;
    }

    RenderSurfaceHandle VulkanBackend::allocate_surface_slot(const RenderSurfaceCreateInfo &init) {
        SurfaceState state{};
        state.descriptor = init.descriptor;
        state.extent = init.framebuffer_extent;
        state.frames_in_flight = sanitize_frames_in_flight(init.desired_frames_in_flight);
        state.active = true;
        state.swapchain_dirty = true;

        for (usize index = 0; index < surfaces_.size(); ++index) {
            if (!surfaces_[index].active) {
                state.generation = next_generation(surfaces_[index].generation);
                surfaces_[index] = state;
                return RenderSurfaceHandle{static_cast<u32>(index), state.generation};
            }
        }

        state.generation = 1;
        surfaces_.push_back(state);
        return RenderSurfaceHandle{static_cast<u32>(surfaces_.size() - 1), state.generation};
    }

} // namespace SFT::Core::Vulkan

namespace SFT::Core {

    unique_ptr<EngineBackend> create_vulkan_backend() {
        return EngineBackend::create<Vulkan::VulkanBackend>();
    }

} // namespace SFT::Core
