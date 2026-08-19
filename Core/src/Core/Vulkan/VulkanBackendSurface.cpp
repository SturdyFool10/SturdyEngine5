

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

#include <Foundation/Foundation.hpp>

#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanConstants.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanHelpers.hpp>
#include <Core/Vulkan/VulkanSurface.hpp>
#include <Core/Vulkan/VulkanSwapchain.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <Core/RenderSurface.hpp>
#include <WindowManager/WindowManager.hpp>

#include <tracy/Tracy.hpp>

using SFT::WindowManager::Window;
using SFT::WindowManager::WindowId;
using std::bad_alloc;
using std::format;
using std::unexpected;

namespace SFT::Core::Vulkan {

    namespace {


        /// Converts the backend-specific value to the corresponding RHI representation.
        ///
        /// @param system `system` value used by the operation.
        ///
        /// @return Returns the value converted to RHI window system representation.
        /// @note This function does not throw exceptions.
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

    /// Performs the surface slot operation for `Vulkan` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    VulkanSurface *VulkanBackend::surface_slot(RenderSurfaceHandle handle) noexcept {
        ZoneScopedN("VulkanBackend::surface_slot");
        if (!handle.is_valid()) {
            return nullptr;
        }
        auto it = surfaces_.find(handle.window_id);
        return it != surfaces_.end() ? &it->second : nullptr;
    }

    /// Performs the surface slot operation for `Vulkan` using the supplied arguments.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    const VulkanSurface *VulkanBackend::surface_slot(RenderSurfaceHandle handle) const noexcept {
        ZoneScopedN("VulkanBackend::surface_slot");
        if (!handle.is_valid()) {
            return nullptr;
        }
        auto it = surfaces_.find(handle.window_id);
        return it != surfaces_.end() ? &it->second : nullptr;
    }

    /// Performs the destroy surface operation for `Vulkan` using the supplied arguments.
    ///
    /// @param surface Surface used or affected by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanBackend::destroySurface(VulkanSurface &surface) noexcept {
        ZoneScopedN("VulkanBackend::destroySurface");
        if (surface.rhi_surface() && rhiDevice) {
            rhiDevice->destroy_surface(surface.rhi_surface());
            surface.clear_rhi_surface();
        }
        surface.destroy(vulkan_instance);
    }

    /// Destroys the all surfaces identified by the supplied parameters.
    ///
    /// @return Returns the current destroy all surfaces value.
    /// @note This function does not throw exceptions.
    void VulkanBackend::destroy_all_surfaces() noexcept {
        ZoneScopedN("VulkanBackend::destroy_all_surfaces");
        for (VulkanSurface &surface : surfaces_ | std::views::values) {
            destroySurface(surface);
        }
        surfaces_.clear();
    }

    /// Destroys the window surface identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanBackend::destroy_window_surface(RenderSurfaceHandle handle) noexcept {
        ZoneScopedN("VulkanBackend::destroy_window_surface");
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

    /// Handles the on surface resize needed callback and updates the associated platform state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param extent `extent` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanBackend::on_surface_resize_needed(RenderSurfaceHandle surface, Extent2D extent) noexcept {
        ZoneScopedN("VulkanBackend::on_surface_resize_needed");
        VulkanSurface *s = surface_slot(surface);
        if (!s) [[unlikely]]
            return;
        s->mark_dirty();
        s->set_extent(extent);


    }

    /// Performs the surface create info from window operation for `Vulkan` using the supplied arguments.
    ///
    /// @param window Window used or affected by the operation.
    /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`.
    RendererExpected<VulkanBackend::SurfaceCreateInfo>
    VulkanBackend::surface_create_info_from_window(Window &window, u32 desired_frames_in_flight) const {
        ZoneScopedN("VulkanBackend::surface_create_info_from_window");
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
        info.framebuffer_extent = *framebuffer;
        info.desired_frames_in_flight = sanitize_frames_in_flight(desired_frames_in_flight);
        return info;
    }

    /// Performs the create surface operation for `Vulkan` using the supplied arguments.
    ///
    /// @param init `init` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`, `GraphicsBackendErrorCode::OutOfMemory`.
    /// @note Allocation failure is converted to the implementation's out-of-memory error/status rather than escaping as `std::bad_alloc`.
    RendererExpected<RenderSurfaceHandle> VulkanBackend::createSurface(const SurfaceCreateInfo &init) {
        ZoneScopedN("VulkanBackend::createSurface");
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
                             init.framebuffer_extent.x,
                             init.framebuffer_extent.y);
        return RenderSurfaceHandle{window_id};
    }

    /// Resolves the RHI surface associated with the supplied key, handle, or resource.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::OperationFailed`.
    RendererExpected<RHI::SurfaceHandle> VulkanBackend::rhi_surface_for(RenderSurfaceHandle handle) {
        ZoneScopedN("VulkanBackend::rhi_surface_for");
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

    /// Creates a window surface from the supplied parameters.
    ///
    /// @param window Window used or affected by the operation.
    /// @param desired_frames_in_flight `desired_frames_in_flight` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`.
    RendererExpected<RenderSurfaceHandle> VulkanBackend::create_window_surface(Window &window, u32 desired_frames_in_flight) {
        ZoneScopedN("VulkanBackend::create_window_surface");
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
