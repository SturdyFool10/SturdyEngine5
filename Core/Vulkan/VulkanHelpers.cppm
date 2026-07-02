module;
#include <format>
#include <string>
#include <vulkan/vulkan_core.h>

export module Sturdy.Core:VulkanHelpers;

import :RenderSurface;
import Sturdy.Platform;

using SFT::Core::SurfaceProvider;
using SFT::Core::SurfaceSystem;
using SFT::Platform::Windowing::NativeWindowSystem;
using SFT::Platform::Windowing::WindowBackendKind;
using std::string;

export namespace SFT::Core::Vulkan {

    [[nodiscard]] inline const char *surface_provider_name(SurfaceProvider provider) noexcept {
        switch (provider) {
            case SurfaceProvider::SDL3:
                return "SDL3";
            case SurfaceProvider::GLFW:
                return "GLFW";
            case SurfaceProvider::Native:
                return "Native";
            default:
                return "Unknown";
        }
    }

    [[nodiscard]] inline const char *surface_system_name(SurfaceSystem system) noexcept {
        switch (system) {
            case SurfaceSystem::Win32:
                return "Win32";
            case SurfaceSystem::X11:
                return "X11";
            case SurfaceSystem::Wayland:
                return "Wayland";
            case SurfaceSystem::Cocoa:
                return "Cocoa";
            default:
                return "Unknown";
        }
    }

    [[nodiscard]] inline string vulkan_format_name(VkFormat fmt) {
#define SFT_VULKAN_FORMAT_NAME(format) \
    case format:                       \
        return #format

        switch (fmt) {
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_UNDEFINED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R4G4_UNORM_PACK8);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R4G4B4A4_UNORM_PACK16);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B4G4R4A4_UNORM_PACK16);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R5G6B5_UNORM_PACK16);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B5G6R5_UNORM_PACK16);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R5G5B5A1_UNORM_PACK16);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B5G5R5A1_UNORM_PACK16);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A1R5G5B5_UNORM_PACK16);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8_SRGB);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8_SRGB);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8_SRGB);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8_SRGB);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8A8_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8A8_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8A8_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8A8_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8A8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8A8_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R8G8B8A8_SRGB);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8A8_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8A8_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8A8_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8A8_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8A8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8A8_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B8G8R8A8_SRGB);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A8B8G8R8_UNORM_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A8B8G8R8_SNORM_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A8B8G8R8_USCALED_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A8B8G8R8_SSCALED_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A8B8G8R8_UINT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A8B8G8R8_SINT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A8B8G8R8_SRGB_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2R10G10B10_UNORM_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2R10G10B10_SNORM_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2R10G10B10_USCALED_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2R10G10B10_SSCALED_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2R10G10B10_UINT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2R10G10B10_SINT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2B10G10R10_UNORM_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2B10G10R10_SNORM_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2B10G10R10_USCALED_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2B10G10R10_SSCALED_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2B10G10R10_UINT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_A2B10G10R10_SINT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16A16_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16A16_SNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16A16_USCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16A16_SSCALED);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16A16_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16A16_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R16G16B16A16_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32B32_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32B32_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32B32_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32B32A32_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32B32A32_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R32G32B32A32_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64B64_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64B64_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64B64_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64B64A64_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64B64A64_SINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_R64G64B64A64_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_B10G11R11_UFLOAT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_D16_UNORM);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_X8_D24_UNORM_PACK32);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_D32_SFLOAT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_S8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_D16_UNORM_S8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_D24_UNORM_S8_UINT);
            SFT_VULKAN_FORMAT_NAME(VK_FORMAT_D32_SFLOAT_S8_UINT);
            default:
                break;
        }

#undef SFT_VULKAN_FORMAT_NAME
        return std::format("VK_FORMAT_UNKNOWN({})", static_cast<int>(fmt));
    }

    [[nodiscard]] inline const char *physical_device_type_name(VkPhysicalDeviceType type) noexcept {
        switch (type) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                return "Discrete";
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                return "Integrated";
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                return "Virtual";
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                return "CPU";
            default:
                return "Other";
        }
    }

    // Maps a PCI vendor ID (VkPhysicalDeviceProperties::vendorID) to a readable name. Vendor IDs
    // are the PCI-assigned values, except software rasterizers which report a Khronos vendor ID.
    [[nodiscard]] inline const char *vendor_name(u32 vendor_id) noexcept {
        switch (vendor_id) {
            case 0x1002:
                return "AMD";
            case 0x10DE:
                return "NVIDIA";
            case 0x8086:
                return "Intel";
            case 0x13B5:
                return "ARM";
            case 0x5143:
                return "Qualcomm";
            case 0x1010:
                return "ImgTec";
            case 0x106B:
                return "Apple";
            case 0x10005:
                return "Mesa";
            default:
                return "Unknown";
        }
    }

    // Decodes VkPhysicalDeviceProperties::driverVersion into a human-readable string. The bit
    // layout is vendor-specific: NVIDIA packs 10.8.8.6, Intel on Windows packs 18.14, and every
    // other vendor uses the standard Vulkan 10.10.12 layout (same as VK_API_VERSION_*).
    [[nodiscard]] inline string format_driver_version(u32 vendor_id, u32 version) {
        if (vendor_id == 0x10DE) { // NVIDIA
            return std::format("{}.{}.{}.{}",
                               (version >> 22) & 0x3FF,
                               (version >> 14) & 0x0FF,
                               (version >> 6) & 0x0FF,
                               version & 0x03F);
        }
#if defined(STURDY_PLATFORM_WINDOWS)
        if (vendor_id == 0x8086) { // Intel (Windows-only encoding)
            return std::format("{}.{}", version >> 14, version & 0x3FFF);
        }
#endif
        return std::format("{}.{}.{}",
                           (version >> 22) & 0x3FF,
                           (version >> 12) & 0x3FF,
                           version & 0xFFF);
    }

    [[nodiscard]] inline SurfaceSystem to_surface_system(NativeWindowSystem system) noexcept {
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

    [[nodiscard]] inline SurfaceProvider to_surface_provider(WindowBackendKind kind) noexcept {
        switch (kind) {
            case WindowBackendKind::SDL3:
                return SurfaceProvider::SDL3;
            case WindowBackendKind::GLFW:
                return SurfaceProvider::GLFW;
        }
        return SurfaceProvider::Unknown;
    }

} // namespace SFT::Core::Vulkan
