#pragma once

#include <Foundation/Foundation.hpp>
#pragma region Imports
#include <format>
#include <vulkan/vulkan_core.h>
#pragma endregion

#include <Core/RenderSurface.hpp>
#include <WindowManager/WindowManager.hpp>

using SFT::Core::SurfaceProvider;
using SFT::Core::SurfaceSystem;
using SFT::WindowManager::NativeWindowSystem;
using SFT::WindowManager::WindowBackendKind;

namespace SFT::Core::Vulkan {

    /// Returns a human-readable name for the supplied surface provider value.
    ///
    /// @param provider `provider` value used by the operation.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *surface_provider_name(SurfaceProvider provider) noexcept;

    /// Returns a human-readable name for the supplied surface system value.
    ///
    /// @param system `system` value used by the operation.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *surface_system_name(SurfaceSystem system) noexcept;

    /// Returns a human-readable name for the supplied vulkan format value.
    ///
    /// @param fmt `fmt` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString vulkan_format_name(VkFormat fmt);

    /// Returns a human-readable name for the supplied physical device type value.
    ///
    /// @param type Type value to inspect, select, or convert.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *physical_device_type_name(VkPhysicalDeviceType type) noexcept;


    /// Returns a human-readable name for the supplied vendor value.
    ///
    /// @param vendor_id Identifier of the target object or resource.
    ///
    /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
    /// @note This function does not throw exceptions.
    [[nodiscard]] const char *vendor_name(u32 vendor_id) noexcept;


    /// Formats driver version using the supplied arguments and current state.
    ///
    /// @param vendor_id Identifier of the target object or resource.
    /// @param version `version` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] UString format_driver_version(u32 vendor_id, u32 version);

    /// Converts the value to surface system representation.
    ///
    /// @param system `system` value used by the operation.
    ///
    /// @return Returns the value converted to surface system representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SurfaceSystem to_surface_system(NativeWindowSystem system) noexcept;

    /// Converts the value to surface provider representation.
    ///
    /// @param kind `kind` value used by the operation.
    ///
    /// @return Returns the value converted to surface provider representation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] SurfaceProvider to_surface_provider(WindowBackendKind kind) noexcept;

} // namespace SFT::Core::Vulkan
