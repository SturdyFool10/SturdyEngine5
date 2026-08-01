// VulkanBackend surface management: window → VkSurfaceKHR creation (SDL3/GLFW providers),
// per-window surface slots, resize notifications, and surface teardown.
#pragma region Imports
#include <glm/ext/vector_float2.hpp>
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

#include <expected>
#include <format>
#include <new>
#include <ranges>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanConstants.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanHelpers.hpp>
#include <Core/Vulkan/VulkanSurface.hpp>
#include <Core/Vulkan/VulkanSwapchain.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <Core/RenderSurface.hpp>
#include <Platform/Platform.hpp>

using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowId;
using std::bad_alloc;
using std::format;
using std::unexpected;

namespace SFT::Core::Vulkan {

    namespace {

        // Core::SurfaceSystem has no separate Xlib/Xcb entry (Platform::Windowing::NativeWindowSystem
        // doesn't distinguish them either — see WindowConfig.hpp) — SDL3/GLFW's X11 native handles are
        // Xlib's (Display*, Window), matching what VulkanRhiDeviceBridge::create_surface's own Xlib
        // branch expects, so X11 maps to RHI::WindowSystem::Xlib here, not Xcb.
        [[nodiscard]] RHI::WindowSystem to_rhi_window_system(SurfaceSystem system) noexcept {
            switch (system) {
                case SurfaceSystem::Win32: return RHI::WindowSystem::Win32;
                case SurfaceSystem::X11: return RHI::WindowSystem::Xlib;
                case SurfaceSystem::Wayland: return RHI::WindowSystem::Wayland;
                case SurfaceSystem::Cocoa: return RHI::WindowSystem::Cocoa;
                case SurfaceSystem::Unknown: return RHI::WindowSystem::Unknown;
            }
            return RHI::WindowSystem::Unknown;
        }

    } // namespace

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
        if (surface.rhi_surface() && rhiDevice) {
            rhiDevice->destroy_surface(surface.rhi_surface());
            surface.clear_rhi_surface();
        }
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

    void VulkanBackend::on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept {
        VulkanSurface *s = surface_slot(surface);
        if (!s) [[unlikely]]
            return;
        s->mark_dirty();
        s->set_extent(extent);
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

        // Surface creation belongs to the concrete window provider. This virtual call keeps Core
        // independent of SDL/GLFW helper symbols while preserving each provider's cross-platform WSI
        // behavior (including Cocoa/Metal-layer setup that cannot be reconstructed from an NSWindow).
        VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
        auto created = init.window->create_vulkan_surface(
            static_cast<void *>(vulkan_instance),
            nullptr,
            &vk_surface);
        if (!created) {
            return unexpected(GraphicsBackendError{
                GraphicsBackendErrorCode::InitializationFailed,
                format("Window provider failed to create a Vulkan surface: {}", created.error().message),
            });
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

    RendererExpected<RHI::SurfaceHandle> VulkanBackend::rhi_surface_for(RenderSurfaceHandle handle) {
        VulkanSurface *surface = surface_slot(handle);
        if (surface == nullptr || !surface->is_active()) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::OperationFailed,
                                                  "Cannot get an RHI surface for an unknown or inactive Vulkan surface."});
        }
        if (surface->rhi_surface()) {
            return surface->rhi_surface();
        }
        if (!rhiDevice) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::OperationFailed,
                                                  "Vulkan RHI bridge is not initialized."});
        }

        auto *bridge = dynamic_cast<VulkanRhiDeviceBridge *>(rhiDevice.get());
        if (bridge == nullptr) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::OperationFailed,
                                                  "Vulkan backend RHI device is not the Vulkan bridge."});
        }

        const RenderSurfaceDescriptor &native_descriptor = surface->descriptor();
        const RHI::SurfaceDesc rhi_surface_desc{
            .system = to_rhi_window_system(native_descriptor.system),
            .display = native_descriptor.display,
            .window = native_descriptor.window,
        };
        auto imported = bridge->import_surface(surface->vk_handle(), rhi_surface_desc);
        if (!imported) {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::OperationFailed,
                                                  imported.error().message});
        }
        surface->set_rhi_surface(*imported);
        return *imported;
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
