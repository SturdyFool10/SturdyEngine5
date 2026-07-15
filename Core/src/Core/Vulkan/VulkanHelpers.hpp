#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include <format>
#include <vulkan/vulkan_core.h>
#pragma endregion

#include <Core/RenderSurface.hpp>
#include <Platform/Platform.hpp>

using SFT::Core::SurfaceProvider;
using SFT::Core::SurfaceSystem;
using SFT::Platform::Windowing::NativeWindowSystem;
using SFT::Platform::Windowing::WindowBackendKind;

namespace SFT::Core::Vulkan {

    [[nodiscard]] const char *surface_provider_name(SurfaceProvider provider) noexcept;

    [[nodiscard]] const char *surface_system_name(SurfaceSystem system) noexcept;

    [[nodiscard]] UString vulkan_format_name(VkFormat fmt);

    [[nodiscard]] const char *physical_device_type_name(VkPhysicalDeviceType type) noexcept;

    // Maps a PCI vendor ID (VkPhysicalDeviceProperties::vendorID) to a readable name. Vendor IDs
    // are the PCI-assigned values, except software rasterizers which report a Khronos vendor ID.
    [[nodiscard]] const char *vendor_name(u32 vendor_id) noexcept;

    // Decodes VkPhysicalDeviceProperties::driverVersion into a human-readable string. The bit
    // layout is vendor-specific: NVIDIA packs 10.8.8.6, Intel on Windows packs 18.14, and every
    // other vendor uses the standard Vulkan 10.10.12 layout (same as VK_API_VERSION_*).
    [[nodiscard]] UString format_driver_version(u32 vendor_id, u32 version);

    [[nodiscard]] SurfaceSystem to_surface_system(NativeWindowSystem system) noexcept;

    [[nodiscard]] SurfaceProvider to_surface_provider(WindowBackendKind kind) noexcept;

} // namespace SFT::Core::Vulkan
