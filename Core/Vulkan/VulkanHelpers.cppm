module;
#include <vulkan/vulkan_core.h>

export module Sturdy.Core:VulkanHelpers;

import :RenderSurface;
import Sturdy.Platform;

using SFT::Core::SurfaceProvider;
using SFT::Core::SurfaceSystem;
using SFT::Platform::Windowing::NativeWindowSystem;
using SFT::Platform::Windowing::WindowBackendKind;

export namespace SFT::Core::Vulkan {

    [[nodiscard]] inline const char *surface_provider_name(SurfaceProvider provider) noexcept {
        switch (provider) {
            case SurfaceProvider::SDL3:   return "SDL3";
            case SurfaceProvider::GLFW:   return "GLFW";
            case SurfaceProvider::Native: return "Native";
            default:                      return "Unknown";
        }
    }

    [[nodiscard]] inline const char *surface_system_name(SurfaceSystem system) noexcept {
        switch (system) {
            case SurfaceSystem::Win32:   return "Win32";
            case SurfaceSystem::X11:     return "X11";
            case SurfaceSystem::Wayland: return "Wayland";
            case SurfaceSystem::Cocoa:   return "Cocoa";
            default:                     return "Unknown";
        }
    }

    [[nodiscard]] inline const char *physical_device_type_name(VkPhysicalDeviceType type) noexcept {
        switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete";
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual";
            case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
            default:                                     return "Other";
        }
    }

    [[nodiscard]] inline SurfaceSystem to_surface_system(NativeWindowSystem system) noexcept {
        switch (system) {
            case NativeWindowSystem::Win32:   return SurfaceSystem::Win32;
            case NativeWindowSystem::X11:     return SurfaceSystem::X11;
            case NativeWindowSystem::Wayland: return SurfaceSystem::Wayland;
            case NativeWindowSystem::Cocoa:   return SurfaceSystem::Cocoa;
            default:                          return SurfaceSystem::Unknown;
        }
    }

    [[nodiscard]] inline SurfaceProvider to_surface_provider(WindowBackendKind kind) noexcept {
        switch (kind) {
            case WindowBackendKind::SDL3: return SurfaceProvider::SDL3;
            case WindowBackendKind::GLFW: return SurfaceProvider::GLFW;
        }
        return SurfaceProvider::Unknown;
    }

} // namespace SFT::Core::Vulkan
