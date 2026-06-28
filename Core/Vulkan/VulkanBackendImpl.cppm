module;

#include <expected>
#include <format>
#include <memory>
#include <new>
#include "volk.h"

export module Sturdy.Core:VulkanBackendImpl;

import :VulkanBackend;
import :RendererError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;
import Sturdy.Platform;

using std::bad_alloc;
using std::format;
using std::unexpected;
using std::unique_ptr;
using SFT::Platform::Windowing::NativeWindowSystem;
using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowBackendKind;

namespace SFT::Core::Vulkan {
    VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
        void* pUserData
    ) {
        (void)type;
        (void)pUserData;

        switch(severity) {

            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
                Foundation::log_debug("[VULKAN API]: {}", pCallbackData->pMessage);
            break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
                Foundation::log_trace("[VULKAN API]: {}", pCallbackData->pMessage);
            break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
                Foundation::log_warn("[VULKAN API]: {}", pCallbackData->pMessage);
            break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
                Foundation::log_error("[VULKAN API]: {}", pCallbackData->pMessage);
            break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT:
                Foundation::log_error("[VULKAN API]: {}", pCallbackData->pMessage);
            break;
        }
        return VK_FALSE;
    }
    namespace {

        [[nodiscard]] u32 sanitize_frames_in_flight(u32 requested) noexcept {
            return requested == 0 ? 2u : requested;
        }

        [[nodiscard]] u32 next_generation(u32 current) noexcept {
            ++current;
            return current == 0 ? 1u : current;
        }

        [[nodiscard]] SurfaceSystem to_surface_system(NativeWindowSystem system) noexcept {
            switch (system) {
                case NativeWindowSystem::Win32:
                    return SurfaceSystem::Win32;
                case NativeWindowSystem::X11:
                    return SurfaceSystem::X11;
                case NativeWindowSystem::Wayland:
                    return SurfaceSystem::Wayland;
                case NativeWindowSystem::Cocoa:
                    return SurfaceSystem::Cocoa;
                default:
                    return SurfaceSystem::Unknown;
            }
        }

        [[nodiscard]] SurfaceProvider to_surface_provider(WindowBackendKind kind) noexcept {
            switch (kind) {
                case WindowBackendKind::SDL3:
                    return SurfaceProvider::SDL3;
                case WindowBackendKind::GLFW:
                    return SurfaceProvider::GLFW;
            }
            return SurfaceProvider::Unknown;
        }

    } // namespace

    VulkanBackend::VulkanBackend(ConstructorKey key)
        : EngineBackend(key) {
    }

    VulkanBackend::~VulkanBackend() {
        // RAII teardown: drain all in-flight work first, then release GPU objects in
        // reverse creation order (surfaces → swapchains → device → allocator → instance).
        wait_idle();
        destroy_all_surfaces();
        // TODO(renderer): destroy the device and instance.
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initialize(const RendererCreateInfo &init) {
        create_info_ = init;
        window_ = init.window;

        if (!window_) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "Vulkan backend requires a window to create its primary surface."});
        }

        // Mark initialized before initVulkan so it can create the backend-owned primary surface
        // during bring-up (the surface is needed to query present support). Reset on failure.
        initialized_ = true;

        auto primary_surface = this->initVulkan(init);
        if (!primary_surface.has_value()) [[unlikely]] {
            initialized_ = false;
            return renderer_error(primary_surface.error().code,
                                  format("Initializing Vulkan has failed: {}", primary_surface.error().message));
        }

        return *primary_surface;
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initVulkan(const RendererCreateInfo &init) {
        if (auto result = this->createVulkanInstance(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create Vulkan instance: {}", result.error().message));
        }

        auto surface_info = surface_create_info_from_window(*this->window_, init.features.desired_frames_in_flight);
        if (!surface_info) [[unlikely]] {
            return unexpected(surface_info.error());
        }

        auto surface = createSurface(*surface_info);
        if (!surface) [[unlikely]] {
            return unexpected(surface.error());
        }



        if (auto result = this->findPhysicalDevice(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to find physical GPU: {}", result.error().message));
        }

        if (auto result = this->discoverGraphicsQueue(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to discover a valid graphics queue: {}", result.error().message));
        }

        return surface;
    }

    RendererResult VulkanBackend::createVulkanInstance(const RendererCreateInfo &init) {
        (void)init; //we plan to use this param later, silencing the warning though
        if (auto res = volkInitialize(); res != VK_SUCCESS) {
            return renderer_error(RendererErrorCode::OperationFailed, format("Volk failed to initialize"));
        }

        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wmissing-field-initializers"
        //pedantic warnings turn no pNext into a warning, but its valid API usage so we ignore that warning
        VkApplicationInfo appInfo {
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "SturdyEngine Application",
            .apiVersion = VK_API_VERSION_1_4 //[IMPORTANT] we define our vulkan version here, we need 1.4
        };


        auto extension_res = window_->required_vulkan_instance_extensions();
        std::vector<const char*> extensions;
        if (!extension_res) [[unlikely]] {
            return renderer_error(RendererErrorCode::OperationFailed, "Failed to get Window extensions list for Vulkan");
        }
        extensions = extension_res.value();
        std::vector<const char*> requestedLayers{};

        #ifdef DEBUG
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); //we enable this for debug builds only...
            layers.push_back("VK_LAYER_KHRONOS_validation");
            const auto severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        #else
            const auto severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        #endif
        //more missing field warnings here, so we extend the warning suppression
        VkDebugUtilsMessengerCreateInfoEXT debugInfo {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = severity,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debugCallback
        };


        VkInstanceCreateInfo instCreateInfo {
          .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
          .pNext = &debugInfo,
          .pApplicationInfo = &appInfo,
          .enabledLayerCount = static_cast<u32>(requestedLayers.size()),
          .ppEnabledLayerNames = requestedLayers.data(),
          .enabledExtensionCount = static_cast<u32>(extensions.size()),
          .ppEnabledExtensionNames = extensions.data()
        };
        #pragma clang diagnostic pop
        if (vkCreateInstance(&instCreateInfo, nullptr, &this->vulkan_instance) != VK_SUCCESS) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed, "vkCreateInstance failed");
        }

        volkLoadInstance(this->vulkan_instance);
        Foundation::log_info("Vulkan Instance Created...");
        return {}; //this is the success case
    }

    RendererResult VulkanBackend::findPhysicalDevice(const RendererCreateInfo &init) {
        (void)init;
        // TODO(renderer): enumerate physical devices (vkEnumeratePhysicalDevices), score them by
        // required device extensions (VK_KHR_swapchain, dynamic rendering, synchronization2),
        // queue-family support, and limits, then select the best and record it for device creation.
        return {};
    }

    RendererResult VulkanBackend::discoverGraphicsQueue(const RendererCreateInfo &init) {
        (void)init;
        // TODO(renderer): query queue-family properties on the chosen physical device and pick a
        // graphics-capable family (plus present/compute/transfer as needed), recording the indices
        // for logical-device and queue creation.
        return {};
    }

    RendererExpected<VulkanBackend::SurfaceCreateInfo> VulkanBackend::surface_create_info_from_window(
        Window &window,
        u32 desired_frames_in_flight) const {

        const auto native = window.native_window_handle();
        if (!native) [[unlikely]] {
            return unexpected(RendererError{
                RendererErrorCode::InitializationFailed,
                format("Failed to query native window handle for Vulkan surface: {}", native.error().message),
            });
        }

        const auto provider_window = window.native_backend_handle();
        if (!provider_window) [[unlikely]] {
            return unexpected(RendererError{
                RendererErrorCode::InitializationFailed,
                format("Failed to query native backend handle for Vulkan surface: {}", provider_window.error().message),
            });
        }

        const auto framebuffer = window.framebuffer_size();
        if (!framebuffer) [[unlikely]] {
            return unexpected(RendererError{
                RendererErrorCode::InitializationFailed,
                format("Failed to query framebuffer size for Vulkan surface: {}", framebuffer.error().message),
            });
        }

        SurfaceCreateInfo info{};
        info.window = &window;
        info.descriptor.provider = to_surface_provider(window.backend_kind());
        info.descriptor.system = to_surface_system(native->system);
        info.descriptor.display = native->display;
        info.descriptor.window = native->window;
        info.descriptor.provider_window = *provider_window;
        info.framebuffer_extent = {framebuffer->x, framebuffer->y};
        info.desired_frames_in_flight = sanitize_frames_in_flight(desired_frames_in_flight);
        return info;
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::createSurface(const SurfaceCreateInfo &init) {
        if (!initialized_) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "Vulkan backend must be initialized before creating its owned surface."});
        }

        if (!init.window) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "Vulkan surface creation requires a live window."});
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

    void VulkanBackend::on_surface_resize_needed(RenderSurfaceHandle surface) noexcept {
        SurfaceState *state = surface_state(surface);
        if (!state) [[unlikely]] {
            return;
        }

        state->swapchain_dirty = true;
        refresh_surface_extent(*state);
        // Swapchain rebuild is deferred to the next render_frame call.
        // Resize-to-zero (minimized) is valid — render_frame will skip presentation.
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

        if (state->swapchain_dirty) {
            refresh_surface_extent(*state);
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

    void VulkanBackend::refresh_surface_extent(SurfaceState &state) noexcept {
        if (!state.window) [[unlikely]] {
            state.extent = {};
            return;
        }

        if (auto framebuffer = state.window->framebuffer_size()) {
            state.extent = {framebuffer->x, framebuffer->y};
        } else {
            state.extent = {};
        }
    }

    void VulkanBackend::release_surface_state(SurfaceState &state) noexcept {
        // TODO(renderer): destroy swapchain, per-frame sync objects, and VkSurfaceKHR.
        const u32 prev_generation = state.generation;
        state = SurfaceState{};
        state.generation = prev_generation;
    }

    void VulkanBackend::destroy_all_surfaces() noexcept {
        for (usize i = surfaces_.size(); i > 0; --i) {
            SurfaceState &state = surfaces_[i - 1];
            if (state.active) {
                release_surface_state(state);
            }
        }
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

    RenderSurfaceHandle VulkanBackend::allocate_surface_slot(const SurfaceCreateInfo &init) {
        SurfaceState state{};
        state.window = init.window;
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
