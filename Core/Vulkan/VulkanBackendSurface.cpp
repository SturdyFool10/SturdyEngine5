// VulkanBackend surface management: window → VkSurfaceKHR creation (SDL3/GLFW providers),
// per-window surface slots, resize notifications, and surface teardown.
module;
#pragma region Imports
#include "glm/ext/vector_float2.hpp"
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

#include <expected>
#include <format>
#include <new>
#include <ranges>
// SDL3 and GLFW surface helpers — included after volk so VkInstance/VkSurfaceKHR are already
// defined. GLFW gates glfwCreateWindowSurface behind #if defined(VK_VERSION_1_0) which volk sets;
// we don't define GLFW_INCLUDE_VULKAN to avoid a double-include of vulkan.h.
#include <GLFW/glfw3.h>
#include <SDL3/SDL_vulkan.h>
#pragma endregion

module Sturdy.Core;

import :VulkanBackend;
import :VulkanConstants;
import :VulkanHelpers;
import :VulkanSurface;
import :VulkanSwapchain;
import :GraphicsBackendError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;
import Sturdy.Platform;

using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowId;
using std::bad_alloc;
using std::format;
using std::unexpected;

namespace SFT::Core::Vulkan {

    VulkanSurface *VulkanBackend::surface_slot(RenderSurfaceHandle handle) noexcept {
        if (!handle.is_valid()) {
            return nullptr;
        }
        auto it = surfaces_.find(handle.window_id);
        return it != surfaces_.end() ? &it->second : nullptr;
    }

    const VulkanSurface *VulkanBackend::surface_slot(RenderSurfaceHandle handle) const noexcept {
        if (!handle.is_valid()) {
            return nullptr;
        }
        auto it = surfaces_.find(handle.window_id);
        return it != surfaces_.end() ? &it->second : nullptr;
    }

    void VulkanBackend::destroySurface(VulkanSurface &surface) noexcept {
        surface.destroy(vulkan_instance);
    }

    void VulkanBackend::destroy_all_surfaces() noexcept {
        for (VulkanSurface &surface : surfaces_ | std::views::values) {
            destroySurface(surface);
        }
        surfaces_.clear();
    }

    void VulkanBackend::destroy_window_surface(RenderSurfaceHandle handle) noexcept {
        if (!handle.is_valid()) [[unlikely]] {
            return;
        }
        auto it = surfaces_.find(handle.window_id);
        if (it == surfaces_.end()) [[unlikely]] {
            return;
        }
        destroySurface(it->second);
        surfaces_.erase(it);
    }

    void VulkanBackend::on_surface_resize_needed(RenderSurfaceHandle surface) noexcept {
        VulkanSurface *s = surface_slot(surface);
        if (!s) [[unlikely]]
            return;
        s->mark_dirty();
        s->refresh_extent();
        // Swapchain rebuild is deferred to the next render_frame call.
        // Resize-to-zero (minimized) is valid — render_frame will skip presentation.
    }

    RendererExpected<VulkanBackend::SurfaceCreateInfo>
    VulkanBackend::surface_create_info_from_window(Window &window, u32 desired_frames_in_flight) const {
        const auto native = window.native_window_handle();
        if (!native) [[unlikely]] {
            return unexpected(GraphicsBackendError{
                GraphicsBackendErrorCode::InitializationFailed,
                format("Failed to query native window handle for Vulkan surface: {}", native.error().message),
            });
        }

        const auto provider_window = window.native_backend_handle();
        if (!provider_window) [[unlikely]] {
            return unexpected(GraphicsBackendError{
                GraphicsBackendErrorCode::InitializationFailed,
                format("Failed to query native backend handle for Vulkan surface: {}", provider_window.error().message),
            });
        }

        const auto framebuffer = window.framebuffer_size();
        if (!framebuffer) [[unlikely]] {
            return unexpected(GraphicsBackendError{
                GraphicsBackendErrorCode::InitializationFailed,
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
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                            "Vulkan backend must be initialized before creating its owned surface."});
        }
        if (!init.window) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                            "Vulkan surface creation requires a live window."});
        }

        const WindowId window_id = init.window->id();
        if (!surfaces_.empty() && surfaces_.contains(window_id)) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                            "A Vulkan surface already exists for this window."});
        }
        if (surfaces_.empty()) {
            surfaces_.reserve(1);
        }

        // Create the platform-specific VkSurfaceKHR.
        VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
        switch (init.descriptor.provider) {
            case SurfaceProvider::SDL3:
                {
                    auto *sdl_window = static_cast<SDL_Window *>(init.descriptor.provider_window);
                    if (!SDL_Vulkan_CreateSurface(sdl_window, vulkan_instance, nullptr, &vk_surface)) {
                        return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                                        format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError())});
                    }
                    break;
                }
            case SurfaceProvider::GLFW:
                {
                    auto *glfw_window = static_cast<GLFWwindow *>(init.descriptor.provider_window);
                    if (glfwCreateWindowSurface(vulkan_instance, glfw_window, nullptr, &vk_surface) != VK_SUCCESS) {
                        return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                                        "glfwCreateWindowSurface failed."});
                    }
                    break;
                }
            default:
                return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                                "Unsupported surface provider; only SDL3 and GLFW are implemented."});
        }

        // VulkanSurface's move constructor is noexcept, so a bad_alloc here can only come from the
        // map's own node allocation, before vulkan_surface is moved from — it still owns vk_surface
        // when the catch block runs, so tearing it down through the wrapper is safe.
        VulkanSurface vulkan_surface(vk_surface, init.descriptor, init.window, init.framebuffer_extent, sanitize_frames_in_flight(init.desired_frames_in_flight));
        try {
            surfaces_.emplace(window_id, std::move(vulkan_surface));
        } catch (const bad_alloc &) {
            vulkan_surface.destroy(vulkan_instance);
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::OutOfMemory,
                                            "Out of memory allocating a Vulkan render surface slot."});
        }

        Foundation::log_info("Vulkan surface created: provider={} system={} extent={}x{}",
                             surface_provider_name(init.descriptor.provider),
                             surface_system_name(init.descriptor.system),
                             init.framebuffer_extent.width,
                             init.framebuffer_extent.height);
        return RenderSurfaceHandle{window_id};
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::create_window_surface(Window &window, u32 desired_frames_in_flight) {
        if (!initialized_) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                            "Vulkan backend must be initialized before adding another window."});
        }

        auto surface_info = surface_create_info_from_window(window, desired_frames_in_flight);
        if (!surface_info) [[unlikely]] {
            return unexpected(surface_info.error());
        }

        auto surface = createSurface(*surface_info);
        if (!surface) [[unlikely]] {
            return unexpected(surface.error());
        }

        return surface;
    }

} // namespace SFT::Core::Vulkan
