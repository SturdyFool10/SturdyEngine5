module;

#include <expected>
#include <memory>
#include <new>

export module Sturdy.Core:VulkanBackendImpl;

import :VulkanBackend;
import :RendererError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;

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
        // RAII teardown: drain all in-flight work first, then release GPU objects in
        // reverse creation order (surfaces → swapchains → device → allocator → instance).
        wait_idle();
        // TODO(renderer): destroy all active surfaces, then the device and instance.
    }

    RendererResult VulkanBackend::initialize(const RendererCreateInfo &init) {
        create_info_ = init;

        // Hard-coded until real VkPhysicalDevice queries exist.
        capabilities_ = RendererCapabilities{};
        capabilities_.multithreaded_command_recording = true;
        capabilities_.timeline_semaphores = true;
        capabilities_.max_frames_in_flight = sanitize_frames_in_flight(init.features.desired_frames_in_flight);

        // TODO(renderer): bring up backend-global Vulkan state in this order:
        //   1. volkInitialize() / load the Vulkan loader.
        //   2. VkInstance with WSI extensions for create_info_.initial_surface_provider/system,
        //      plus VK_EXT_debug_utils in Debug builds.
        //   3. Select a Vulkan 1.4 physical device (dynamic rendering, sync2, timeline semaphores).
        //   4. Create the logical device + VMA allocator; volkLoadDevice(device).

        initialized_ = true;
        return {};
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::create_surface(const RenderSurfaceCreateInfo &init) {
        if (!initialized_) {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "Vulkan backend must be initialized before creating a surface."});
        }

        try {
            RenderSurfaceHandle handle = allocate_surface_slot(init);

            // TODO(renderer): allocate per-surface GPU resources:
            //   1. VkSurfaceKHR via init.descriptor.provider_window (SDL_Vulkan_CreateSurface,
            //      glfwCreateWindowSurface) or native WSI handles for a headless path.
            //   2. Verify the chosen queues can present to this surface.
            //   3. Create the swapchain and per-frame sync objects when extent is non-zero.
            return handle;
        } catch (const bad_alloc &) {
            return unexpected(RendererError{RendererErrorCode::OutOfMemory,
                                            "Out of memory while allocating a Vulkan render surface slot."});
        }
    }

    RendererResult VulkanBackend::destroy_surface(RenderSurfaceHandle surface) {
        SurfaceState *state = surface_state(surface);
        if (!state) {
            return renderer_error(RendererErrorCode::SurfaceLost, "Invalid Vulkan render surface handle.");
        }

        // Drain all in-flight work before releasing any GPU resources. Full device wait is
        // acceptable here; once per-surface timeline semaphores exist this can be tightened.
        wait_idle();

        // TODO(renderer): destroy swapchain, per-frame sync objects, and VkSurfaceKHR.
        const u32 prev_generation = state->generation;
        *state = SurfaceState{};
        state->generation = prev_generation;
        return {};
    }

    void VulkanBackend::on_resize(RenderSurfaceHandle surface, Extent2D new_extent) noexcept {
        SurfaceState *state = surface_state(surface);
        if (!state) {
            return;
        }

        state->extent = new_extent;
        state->swapchain_dirty = true;
        // Swapchain rebuild is deferred to the next render_frame call.
        // Passing zero extent (minimized) is valid — render_frame will skip presentation.
    }

    RendererCapabilities VulkanBackend::capabilities() const noexcept {
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
            // TODO(renderer): wait for this surface's in-flight frames, recreate the swapchain
            // and all swapchain-sized attachments, then clear the flag only on success.
            state->swapchain_dirty = false;
        }

        // TODO(renderer): acquire → record (Vulkan 1.4 dynamic rendering) → submit
        // (synchronization2 / vkQueueSubmit2) → present. On VK_ERROR_OUT_OF_DATE_KHR or
        // VK_SUBOPTIMAL_KHR, set swapchain_dirty and retry next frame.
        return {};
    }

    void VulkanBackend::wait_idle() noexcept {
        // TODO(renderer): vkDeviceWaitIdle(device_) once the logical device exists.
    }

    VulkanBackend::SurfaceState *VulkanBackend::surface_state(RenderSurfaceHandle handle) noexcept {
        if (!handle.is_valid() || static_cast<usize>(handle.index) >= surfaces_.size()) {
            return nullptr;
        }

        SurfaceState &state = surfaces_[static_cast<usize>(handle.index)];
        return (state.active && state.generation == handle.generation) ? &state : nullptr;
    }

    const VulkanBackend::SurfaceState *VulkanBackend::surface_state(RenderSurfaceHandle handle) const noexcept {
        if (!handle.is_valid() || static_cast<usize>(handle.index) >= surfaces_.size()) {
            return nullptr;
        }

        const SurfaceState &state = surfaces_[static_cast<usize>(handle.index)];
        return (state.active && state.generation == handle.generation) ? &state : nullptr;
    }

    RenderSurfaceHandle VulkanBackend::allocate_surface_slot(const RenderSurfaceCreateInfo &init) {
        SurfaceState state{};
        state.descriptor = init.descriptor;
        state.extent = init.framebuffer_extent;
        state.frames_in_flight = sanitize_frames_in_flight(init.desired_frames_in_flight);
        state.active = true;
        state.swapchain_dirty = true;

        for (usize i = 0; i < surfaces_.size(); ++i) {
            if (!surfaces_[i].active) {
                state.generation = next_generation(surfaces_[i].generation);
                surfaces_[i] = state;
                return RenderSurfaceHandle{static_cast<u32>(i), state.generation};
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
