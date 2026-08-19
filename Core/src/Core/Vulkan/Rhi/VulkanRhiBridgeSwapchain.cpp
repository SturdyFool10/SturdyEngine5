#include <Core/GraphicsPlatform/RhiBridge.hpp>

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#if defined(__linux__)
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR


#define Window X11Window
#include <X11/Xlib.h>
#include <wayland-client.h>
#include <xcb/xcb.h>


#if defined(Success)
#undef Success
#endif
#if defined(None)
#undef None
#endif
#if defined(Always)
#undef Always
#endif
#if defined(Bool)
#undef Bool
#endif
#endif
#include "volk.h"
#if defined(__linux__)
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_xcb.h>
#include <vulkan/vulkan_wayland.h>
#undef Window
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/Foundation.hpp>

#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <Core/Vulkan/VulkanSwapchain.hpp>
#include <Core/Vulkan/VulkanSync.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

using std::optional;
using std::span;
using std::string;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        /// Presents the completed frame to the target surface or swapchain.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr VkPresentModeKHR present_mode_to_vk(rhi::PresentMode mode) noexcept {
            switch (mode) {
                case rhi::PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
                case rhi::PresentMode::FifoRelaxed: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
                case rhi::PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
                case rhi::PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;


                case rhi::PresentMode::FifoLatestReady: return VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        /// Performs the Vulkan present mode to RHI operation for `Vulkan` using the supplied arguments.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note Normal inability to produce a value is represented by an empty optional.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::optional<rhi::PresentMode> vk_present_mode_to_rhi(VkPresentModeKHR mode) noexcept {
            switch (mode) {
                case VK_PRESENT_MODE_FIFO_KHR: return rhi::PresentMode::Fifo;
                case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return rhi::PresentMode::FifoRelaxed;
                case VK_PRESENT_MODE_MAILBOX_KHR: return rhi::PresentMode::Mailbox;
                case VK_PRESENT_MODE_IMMEDIATE_KHR: return rhi::PresentMode::Immediate;
                case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR: return rhi::PresentMode::FifoLatestReady;
                default: return std::nullopt;
            }
        }


        /// Performs the supported RHI present modes operation for `Vulkan` using the supplied arguments.
        ///
        /// @param vk_modes `vk_modes` value used by the operation.
        /// @param fifo_latest_ready_enabled `fifo_latest_ready_enabled` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<rhi::PresentMode> supported_rhi_present_modes(span<const VkPresentModeKHR> vk_modes,
                                                                           bool fifo_latest_ready_enabled) {
            vector<rhi::PresentMode> supported;
            supported.reserve(vk_modes.size());
            for (VkPresentModeKHR vk_mode : vk_modes) {
                if (const optional<rhi::PresentMode> mode = vk_present_mode_to_rhi(vk_mode)) {
                    if (*mode == rhi::PresentMode::FifoLatestReady && !fifo_latest_ready_enabled) {
                        continue;
                    }
                    supported.push_back(*mode);
                }
            }
            return supported;
        }


        /// Resolves present mode into the concrete value used by the engine.
        ///
        /// @param vk_modes `vk_modes` value used by the operation.
        /// @param strategy `strategy` value used by the operation.
        /// @param fifo_latest_ready_enabled `fifo_latest_ready_enabled` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] rhi::PresentationResolution resolve_present_mode(span<const VkPresentModeKHR> vk_modes,
                                                                       rhi::PresentStrategy strategy,
                                                                       bool fifo_latest_ready_enabled) {
            const vector<rhi::PresentMode> supported = supported_rhi_present_modes(vk_modes, fifo_latest_ready_enabled);
            const rhi::PresentMode ideal = rhi::present_mode_preference(strategy)[0];
            const rhi::PresentMode effective =
                rhi::choose_present_mode(span<const rhi::PresentMode>{supported.data(), supported.size()}, strategy);
            const bool degraded = effective != ideal;
            if (degraded) {
                Foundation::log_warn(
                    "Presentation strategy {} wanted {} but the surface doesn't support it; using {} instead.",
                    rhi::present_strategy_name(strategy), rhi::present_mode_name(ideal), rhi::present_mode_name(effective));
            }
            return rhi::PresentationResolution{.strategy = strategy, .effective_mode = effective, .degraded = degraded};
        }

        /// Performs the color space to Vulkan operation for `Vulkan` using the supplied arguments.
        ///
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkColorSpaceKHR color_space_to_vk(rhi::ColorSpace color_space) noexcept {
            switch (color_space) {
                case rhi::ColorSpace::SrgbNonlinear: return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                case rhi::ColorSpace::Hdr10St2084: return VK_COLOR_SPACE_HDR10_ST2084_EXT;
                case rhi::ColorSpace::ScrgbLinear: return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
                case rhi::ColorSpace::Hdr10Hlg: return VK_COLOR_SPACE_HDR10_HLG_EXT;


                case rhi::ColorSpace::DolbyVision: return VK_COLOR_SPACE_DOLBYVISION_EXT;
                case rhi::ColorSpace::AdobeRgbLinear: return VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT;
                case rhi::ColorSpace::AdobeRgbNonlinear: return VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT;
                case rhi::ColorSpace::DisplayP3Linear: return VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT;
                case rhi::ColorSpace::DisplayP3Nonlinear: return VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT;
                case rhi::ColorSpace::Bt2020Linear: return VK_COLOR_SPACE_BT2020_LINEAR_EXT;
            }
            return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        }


        /// Performs the requires swapchain colorspace extension operation for `Vulkan` using the supplied arguments.
        ///
        /// @param color_space `color_space` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool requires_swapchain_colorspace_extension(rhi::ColorSpace color_space) noexcept {
            return color_space != rhi::ColorSpace::SrgbNonlinear;
        }

        struct SurfaceFormatSelection {
            VkSurfaceFormatKHR vk{};
            rhi::Format format = rhi::Format::Undefined;
            rhi::ColorSpace color_space = rhi::ColorSpace::SrgbNonlinear;
            bool degraded = false;
        };

        /// Converts a Vulkan swapchain format into the matching RHI format when the renderer can target it.
        ///
        /// @param format Vulkan surface format to convert.
        ///
        /// @return Returns the corresponding RHI format when supported; otherwise returns `std::nullopt`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<rhi::Format> swapchain_format_from_vk(VkFormat format) noexcept {
            switch (format) {
                case VK_FORMAT_B8G8R8A8_SRGB: return rhi::Format::BGRA8UnormSrgb;
                case VK_FORMAT_R8G8B8A8_SRGB: return rhi::Format::RGBA8UnormSrgb;
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return rhi::Format::RGB10A2Unorm;
                case VK_FORMAT_R16G16B16A16_SFLOAT: return rhi::Format::RGBA16Float;
                default: return std::nullopt;
            }
        }

        /// Selects the closest usable surface format while preserving the requested color space when possible.
        ///
        /// @param formats Surface format/color-space pairs exposed by the Vulkan WSI implementation.
        /// @param requested Requested RHI swapchain format.
        /// @param requested_color_space Requested RHI presentation color space.
        ///
        /// @return Returns the selected presentation format and effective RHI interpretation, or `std::nullopt` when none is usable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<SurfaceFormatSelection> choose_surface_format(
            span<const VkSurfaceFormatKHR> formats,
            rhi::Format requested,
            rhi::ColorSpace requested_color_space) noexcept {
            const VkFormat preferred = SFT::Core::Vulkan::to_vk(requested);
            const VkColorSpaceKHR preferred_color_space = color_space_to_vk(requested_color_space);

            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.colorSpace != preferred_color_space) {
                    continue;
                }
                if (format.format == preferred || format.format == VK_FORMAT_UNDEFINED) {
                    return SurfaceFormatSelection{
                        .vk = VkSurfaceFormatKHR{.format = preferred, .colorSpace = preferred_color_space},
                        .format = requested,
                        .color_space = requested_color_space,
                        .degraded = false,
                    };
                }
            }

            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.colorSpace != preferred_color_space) {
                    continue;
                }
                if (const auto mapped = swapchain_format_from_vk(format.format)) {
                    return SurfaceFormatSelection{
                        .vk = format,
                        .format = *mapped,
                        .color_space = requested_color_space,
                        .degraded = format.format != preferred,
                    };
                }
            }

            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    continue;
                }
                if (format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_UNDEFINED) {
                    return SurfaceFormatSelection{
                        .vk = VkSurfaceFormatKHR{
                            .format = VK_FORMAT_B8G8R8A8_SRGB,
                            .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
                        },
                        .format = rhi::Format::BGRA8UnormSrgb,
                        .color_space = rhi::ColorSpace::SrgbNonlinear,
                        .degraded = requested_color_space != rhi::ColorSpace::SrgbNonlinear ||
                                    requested != rhi::Format::BGRA8UnormSrgb,
                    };
                }
            }

            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    continue;
                }
                if (const auto mapped = swapchain_format_from_vk(format.format)) {
                    return SurfaceFormatSelection{
                        .vk = format,
                        .format = *mapped,
                        .color_space = rhi::ColorSpace::SrgbNonlinear,
                        .degraded = requested_color_space != rhi::ColorSpace::SrgbNonlinear || *mapped != requested,
                    };
                }
            }

            return std::nullopt;
        }

        /// Selects image count that best satisfies the supplied requirements.
        ///
        /// @param caps `caps` value used by the operation.
        /// @param requested `requested` value used by the operation.
        ///
        /// @return Returns the requested count or size.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 choose_image_count(const VkSurfaceCapabilitiesKHR &caps, u32 requested) noexcept {
            u32 count = requested == 0 ? caps.minImageCount + 1 : requested;
            count = std::max(count, caps.minImageCount);
            if (caps.maxImageCount > 0) {
                count = std::min(count, caps.maxImageCount);
            }
            return count;
        }

        /// Performs the composite alpha to Vulkan operation for `Vulkan` using the supplied arguments.
        ///
        /// @param mode Mode controlling how the operation is performed.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkCompositeAlphaFlagBitsKHR composite_alpha_to_vk(rhi::CompositeAlphaMode mode) noexcept {
            switch (mode) {
                case rhi::CompositeAlphaMode::Opaque: return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
                case rhi::CompositeAlphaMode::Premultiplied: return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
                case rhi::CompositeAlphaMode::PostMultiplied: return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
                case rhi::CompositeAlphaMode::Inherit: return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
                case rhi::CompositeAlphaMode::Auto: return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            }
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }

        /// Returns a human-readable name for the supplied Vulkan result value.
        ///
        /// @param result `result` value used by the operation.
        ///
        /// @return Returns a pointer to a static null-terminated label; the returned pointer is not owned by the caller.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const char *vk_result_name(VkResult result) noexcept {
            switch (result) {
                case VK_SUCCESS: return "VK_SUCCESS";
                case VK_NOT_READY: return "VK_NOT_READY";
                case VK_TIMEOUT: return "VK_TIMEOUT";
                case VK_EVENT_SET: return "VK_EVENT_SET";
                case VK_EVENT_RESET: return "VK_EVENT_RESET";
                case VK_INCOMPLETE: return "VK_INCOMPLETE";
                case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
                case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
                case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
                case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
                case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
                case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
                case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
                case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
                case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
                case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
                case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
                case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
                case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
                case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
                case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
                case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
                case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
                default: return "unknown VkResult";
            }
        }


        /// Resolves composite alpha into the concrete value used by the engine.
        ///
        /// @param supported `supported` value used by the operation.
        /// @param requested `requested` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] rhi::CompositeAlphaMode resolve_composite_alpha(VkCompositeAlphaFlagsKHR supported,
                                                                     rhi::CompositeAlphaMode requested) noexcept {
            const auto is_supported = [supported](rhi::CompositeAlphaMode mode) noexcept {
                return (supported & composite_alpha_to_vk(mode)) != 0;
            };

            if (requested != rhi::CompositeAlphaMode::Auto && is_supported(requested)) {
                return requested;
            }


            const bool wants_transparency =
                requested != rhi::CompositeAlphaMode::Auto && requested != rhi::CompositeAlphaMode::Opaque;
            if (wants_transparency) {
                for (rhi::CompositeAlphaMode candidate : rhi::transparent_composite_alpha_preference()) {
                    if (is_supported(candidate)) {
                        return candidate;
                    }
                }
            }
            if (is_supported(rhi::CompositeAlphaMode::Opaque)) {
                return rhi::CompositeAlphaMode::Opaque;
            }
            for (rhi::CompositeAlphaMode candidate : rhi::transparent_composite_alpha_preference()) {
                if (is_supported(candidate)) {
                    return candidate;
                }
            }
            return rhi::CompositeAlphaMode::Opaque;
        }


        /// Builds HDR metadata.
        ///
        /// @param hdr_query `hdr_query` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkHdrMetadataEXT build_hdr_metadata(const rhi::SurfaceHdrCapabilityQuery &hdr_query) noexcept {
            VkHdrMetadataEXT metadata{
                .sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT,
                .pNext = nullptr,
                .displayPrimaryRed = VkXYColorEXT{.x = 0.680f, .y = 0.320f},
                .displayPrimaryGreen = VkXYColorEXT{.x = 0.265f, .y = 0.690f},
                .displayPrimaryBlue = VkXYColorEXT{.x = 0.150f, .y = 0.060f},
                .whitePoint = VkXYColorEXT{.x = 0.3127f, .y = 0.3290f},
                .maxLuminance = 1000.0f,
                .minLuminance = 0.001f,
                .maxContentLightLevel = 1000.0f,
                .maxFrameAverageLightLevel = 400.0f,
            };
            if (!hdr_query || !hdr_query.capabilities.display_metadata.has_value()) {
                return metadata;
            }
            const rhi::HdrDisplayMetadata &real = *hdr_query.capabilities.display_metadata;
            metadata.displayPrimaryRed = VkXYColorEXT{.x = real.red_primary.x, .y = real.red_primary.y};
            metadata.displayPrimaryGreen = VkXYColorEXT{.x = real.green_primary.x, .y = real.green_primary.y};
            metadata.displayPrimaryBlue = VkXYColorEXT{.x = real.blue_primary.x, .y = real.blue_primary.y};
            metadata.whitePoint = VkXYColorEXT{.x = real.white_point.x, .y = real.white_point.y};
            if (real.max_luminance_nits > 0.0f) {
                metadata.maxLuminance = real.max_luminance_nits;
                metadata.maxContentLightLevel = real.max_luminance_nits;
            }
            metadata.minLuminance = real.min_luminance_nits;
            if (real.max_full_frame_luminance_nits > 0.0f) {
                metadata.maxFrameAverageLightLevel = real.max_full_frame_luminance_nits;
            }
            return metadata;
        }

    } // namespace

    /// Creates a surface record value from the supplied arguments.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param owns_surface Surface used or affected by the operation.
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    VulkanRhiDeviceBridge::SurfaceRecord VulkanRhiDeviceBridge::make_surface_record(VkSurfaceKHR surface, bool owns_surface,
                                                                                    const rhi::SurfaceDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::make_surface_record");
        SurfaceRecord record{.surface = surface, .owns_surface = owns_surface};
        record.stored_label = desc.label != nullptr ? std::string{desc.label} : std::string{};
        record.desc = rhi::SurfaceDesc{
            .system = desc.system,
            .display = desc.display,
            .window = desc.window,
            .label = record.stored_label.c_str(),
        };
        return record;
    }

    /// Creates a surface from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`, `RhiErrorCode::Unsupported`.
    rhi::RhiExpected<rhi::SurfaceHandle> VulkanRhiDeviceBridge::create_surface(const rhi::SurfaceDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_surface");
        if (instance_ == VK_NULL_HANDLE) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run create_surface: instance resources are not ready.");
        }

        VkSurfaceKHR surface = VK_NULL_HANDLE;
#if defined(__linux__)
        switch (desc.system) {
            case rhi::WindowSystem::Xlib: {
                const auto create_xlib_surface = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(
                    vkGetInstanceProcAddr(instance_, "vkCreateXlibSurfaceKHR"));
                if (create_xlib_surface == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                          "create_surface: VK_KHR_xlib_surface is not enabled or loaded.");
                }
                const VkXlibSurfaceCreateInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
                    .dpy = static_cast<Display *>(desc.display),
                    .window = static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(desc.window)),
                };
                if (create_xlib_surface(instance_, &info, nullptr, &surface) != VK_SUCCESS) {
                    return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "create_surface: vkCreateXlibSurfaceKHR failed.");
                }
                break;
            }
            case rhi::WindowSystem::Xcb: {
                const auto create_xcb_surface = reinterpret_cast<PFN_vkCreateXcbSurfaceKHR>(
                    vkGetInstanceProcAddr(instance_, "vkCreateXcbSurfaceKHR"));
                if (create_xcb_surface == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                          "create_surface: VK_KHR_xcb_surface is not enabled or loaded.");
                }
                const VkXcbSurfaceCreateInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
                    .connection = static_cast<xcb_connection_t *>(desc.display),
                    .window = static_cast<xcb_window_t>(reinterpret_cast<std::uintptr_t>(desc.window)),
                };
                if (create_xcb_surface(instance_, &info, nullptr, &surface) != VK_SUCCESS) {
                    return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "create_surface: vkCreateXcbSurfaceKHR failed.");
                }
                break;
            }
            case rhi::WindowSystem::Wayland: {
                const auto create_wayland_surface = reinterpret_cast<PFN_vkCreateWaylandSurfaceKHR>(
                    vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
                if (create_wayland_surface == nullptr) {
                    return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                          "create_surface: VK_KHR_wayland_surface is not enabled or loaded.");
                }
                const VkWaylandSurfaceCreateInfoKHR info{
                    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
                    .display = static_cast<wl_display *>(desc.display),
                    .surface = static_cast<wl_surface *>(desc.window),
                };
                if (create_wayland_surface(instance_, &info, nullptr, &surface) != VK_SUCCESS) {
                    return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed, "create_surface: vkCreateWaylandSurfaceKHR failed.");
                }
                break;
            }
            default:
                return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                      "create_surface: this WindowSystem is not supported by the Linux Vulkan bridge.");
        }
#else
        (void)desc;
        return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                              "create_surface: raw-native Vulkan RHI surfaces are not compiled into this platform build.");
#endif

        return surfaces_.insert(make_surface_record(surface,                  true, desc));
    }

    /// Imports surface using the supplied arguments and current state.
    ///
    /// @param surface Surface used or affected by the operation.
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<rhi::SurfaceHandle> VulkanRhiDeviceBridge::import_surface(VkSurfaceKHR surface, const rhi::SurfaceDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::import_surface");
        if (surface == VK_NULL_HANDLE) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "import_surface: cannot import a null VkSurfaceKHR.");
        }
        return surfaces_.insert(make_surface_record(surface,                  false, desc));
    }

    /// Destroys the surface identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_surface(rhi::SurfaceHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_surface");
        SurfaceRecord *record = surfaces_.find(handle);
        if (record != nullptr && record->surface != VK_NULL_HANDLE && record->owns_surface) {
            vkDestroySurfaceKHR(instance_, record->surface, nullptr);
        }
        surfaces_.erase(handle);
    }

    /// Creates a swapchain from the supplied parameters.
    ///
    /// @param desc Description of the resource or operation to perform.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`, `RhiErrorCode::Unsupported`, `GraphicsBackendErrorCode::DeviceLost`, `GraphicsBackendErrorCode::OperationFailed`.
    rhi::RhiExpected<rhi::SwapchainHandle> VulkanRhiDeviceBridge::create_swapchain(const rhi::SwapchainDesc &desc) {
        ZoneScopedN("VulkanRhiDeviceBridge::create_swapchain");
        if (logical_device_ == nullptr || physical_device_ == nullptr) {
            return device_not_ready<rhi::SwapchainHandle>("create_swapchain");
        }
        SurfaceRecord *surface = surfaces_.find(desc.surface);
        if (surface == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "create_swapchain: unknown surface handle.");
        }

        SwapchainRecord *old_record = nullptr;
        if (desc.old_swapchain.is_valid()) {
            old_record = swapchains_.find(desc.old_swapchain);
            if (old_record == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_swapchain: old_swapchain is not a live swapchain handle.");
            }
            if (old_record->surface != desc.surface) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "create_swapchain: old_swapchain belongs to a different surface.");
            }
        }

        auto caps = physical_device_->surface_capabilities(surface->surface);
        if (!caps) {
            return rhi_error_from_graphics(caps.error());
        }
        auto formats = physical_device_->surface_formats(surface->surface);
        if (!formats) {
            return rhi_error_from_graphics(formats.error());
        }
        auto modes = physical_device_->surface_present_modes(surface->surface);
        if (!modes) {
            return rhi_error_from_graphics(modes.error());
        }

        const bool requested_color_space_extension_unavailable =
            requires_swapchain_colorspace_extension(desc.color_space) && !hdr_swapchain_colorspace_enabled_;
        const rhi::ColorSpace selectable_color_space = requested_color_space_extension_unavailable
            ? rhi::ColorSpace::SrgbNonlinear
            : desc.color_space;
        const rhi::Format selectable_format = requested_color_space_extension_unavailable
            ? rhi::Format::BGRA8UnormSrgb
            : desc.format;
        const auto selected_format = choose_surface_format(*formats, selectable_format, selectable_color_space);
        if (!selected_format) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_swapchain: surface exposes no usable Vulkan presentation format.");
        }
        const VkSurfaceFormatKHR format = selected_format->vk;
        if (requested_color_space_extension_unavailable || selected_format->degraded) {
            Foundation::log_warn(
                "Vulkan: requested swapchain format/color space is unavailable; using a supported presentation fallback.");
        }
        const VkExtent2D extent{
            .width = desc.width != 0 ? desc.width : caps->currentExtent.width,
            .height = desc.height != 0 ? desc.height : caps->currentExtent.height,
        };
        VkImageUsageFlags usage = to_vk(desc.usage);
        if (usage == 0) {
            usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        usage &= caps->supportedUsageFlags;
        if (usage == 0) {
            usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }

        const bool fifo_latest_ready_enabled = enabled_features_.has(rhi::Feature::PresentModeFifoLatestReady);
        rhi::PresentationResolution resolution =
            resolve_present_mode(*modes, desc.present_strategy, fifo_latest_ready_enabled);
        resolution.degraded = resolution.degraded || requested_color_space_extension_unavailable || selected_format->degraded;
        resolution.effective_format = selected_format->format;
        resolution.effective_color_space = selected_format->color_space;


        bool present_via_compute = false;
        if (desc.allow_present_from_compute && compute_queue_ != nullptr &&
            compute_queue_->family_index() != graphics_queue_->family_index()) {
            present_via_compute = physical_device_->queue_family_supports_present(compute_queue_->family_index(), surface->surface);
            if (present_via_compute) {
                Foundation::log_info(
                    "Swapchain will present from the compute queue (family={}) instead of graphics (family={}).",
                    compute_queue_->family_index(), graphics_queue_->family_index());
            }
        }
        resolution.present_queue_is_compute = present_via_compute;

        const rhi::CompositeAlphaMode effective_composite_alpha =
            resolve_composite_alpha(caps->supportedCompositeAlpha, desc.composite_alpha);
        const bool transparency_requested = desc.composite_alpha != rhi::CompositeAlphaMode::Auto &&
                                            desc.composite_alpha != rhi::CompositeAlphaMode::Opaque;
        resolution.effective_composite_alpha = effective_composite_alpha;
        resolution.composite_alpha_degraded =
            transparency_requested && effective_composite_alpha == rhi::CompositeAlphaMode::Opaque;
        if (resolution.composite_alpha_degraded) {


            Foundation::log_warn(
                "Swapchain requested composite alpha {} for transparent composition, but this surface only "
                "supports {:#x} — falling back to Opaque. The compositor will discard the alpha this "
                "surface renders and the window will composite over black. (Vulkan on Win32 commonly "
                "advertises Opaque only; per-pixel window transparency needs a surface that advertises a "
                "non-opaque mode.)",
                rhi::composite_alpha_mode_name(desc.composite_alpha),
                static_cast<u32>(caps->supportedCompositeAlpha));
        } else if (transparency_requested) {
            Foundation::log_info("Swapchain composite alpha resolved to {} for transparent composition.",
                                 rhi::composite_alpha_mode_name(effective_composite_alpha));
        }


        resolution.supports_completion_fence = enabled_features_.has(rhi::Feature::SwapchainMaintenance);


        if (GraphicsPlatform::composition_present_compiled() && !desc.request_full_screen_exclusive) {


            const VkImageUsageFlags composition_usage =
                (usage & ~static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_STORAGE_BIT)) | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            const u32 composition_image_count = choose_image_count(*caps, desc.image_count);
            const GraphicsPlatform::NativeSurfaceHandle platform_surface{
                .system = ::SFT::Core::to_graphics_platform(surface->desc.system),
                .display = surface->desc.display,
                .window = surface->desc.window,
                .label = surface->desc.label != nullptr ? std::string_view{surface->desc.label} : std::string_view{},
            };


            const GraphicsPlatform::CompositionAlphaMode composition_alpha_mode =
                transparency_requested ? GraphicsPlatform::CompositionAlphaMode::Premultiplied
                                       : GraphicsPlatform::CompositionAlphaMode::Ignore;


            const bool can_resize_in_place = old_record != nullptr && old_record->owns_composition_resources &&
                old_record->composition.presenter != nullptr &&
                old_record->composition_vk_format == format.format &&
                old_record->composition_alpha_mode == composition_alpha_mode &&
                static_cast<u32>(old_record->composition.images.size()) == composition_image_count &&
                (old_record->composition.presenter->width() != extent.width ||
                 old_record->composition.presenter->height() != extent.height);

            RendererExpected<CompositionSwapchainResources> composition = [&]() -> RendererExpected<CompositionSwapchainResources> {
                if (can_resize_in_place) {


                    auto resized = resize_composition_swapchain_resources(
                        logical_device_->vk_handle(), physical_device_->vk_handle(),
                        std::move(old_record->composition), format.format, composition_usage, extent.width,
                        extent.height);
                    if (resized) {
                        return resized;
                    }


                    Foundation::log_warn(
                        "Composition presenter resize-in-place failed ({}); rebuilding from scratch.",
                        resized.error().message);
                }


                if (old_record != nullptr && old_record->owns_composition_resources &&
                    old_record->composition.presenter != nullptr) {
                    const VkResult idle_result = vkDeviceWaitIdle(logical_device_->vk_handle());
                    if (idle_result != VK_SUCCESS) {
                        return graphics_backend_error(
                            idle_result == VK_ERROR_DEVICE_LOST ? GraphicsBackendErrorCode::DeviceLost
                                                                : GraphicsBackendErrorCode::OperationFailed,
                            "Composition presenter replacement could not wait for the Vulkan device to become idle.");
                    }
                    old_record->composition.presenter.reset();
                }
                return create_composition_swapchain_resources(
                    logical_device_->vk_handle(), physical_device_->vk_handle(), platform_surface, format.format,
                    composition_usage, extent.width, extent.height, composition_image_count, composition_alpha_mode);
            }();
            if (composition) {
                SwapchainRecord record{};
                record.surface = desc.surface;
                record.composition = std::move(*composition);
                record.owns_composition_resources = true;
                record.composition_vk_format = format.format;
                record.composition_alpha_mode = composition_alpha_mode;


                resolution.effective_composite_alpha =
                    transparency_requested ? rhi::CompositeAlphaMode::Premultiplied : rhi::CompositeAlphaMode::Opaque;
                resolution.composite_alpha_degraded = false;


                resolution.via_composition_present = true;


                resolution.supports_completion_fence = false;
                record.presentation_resolution = resolution;
                record.present_via_compute = false;
                Foundation::log_info(
                    "Swapchain presenting via composition present (DXGI + DirectComposition) instead of "
                    "vkQueuePresentKHR (composite alpha: {}).",
                    rhi::composite_alpha_mode_name(resolution.effective_composite_alpha));

                const u32 image_count = static_cast<u32>(record.composition.images.size());
                record.textures.reserve(image_count);
                record.views.reserve(image_count);
                record.render_finished_semaphores.reserve(image_count);
                record.image_available_signal_indices.resize(image_count, 0);
                const auto discard_record = [&]() noexcept {
                    for (rhi::TextureViewHandle view : record.views) {
                        texture_views_.erase(view);
                    }
                    for (rhi::TextureHandle texture : record.textures) {
                        textures_.erase(texture);
                    }
                    destroy_composition_swapchain_resources(logical_device_->vk_handle(), record.composition);
                };
                for (u32 i = 0; i < image_count; ++i) {
                    rhi::TextureHandle texture =
                        textures_.insert(TextureRecord{std::move(record.composition.images[i].image), selected_format->format});
                    record.textures.push_back(texture);
                    record.views.push_back(texture_views_.insert(std::move(record.composition.views[i])));

                    auto render_finished = VulkanSemaphore::create_binary(logical_device_->vk_handle());
                    if (!render_finished) {
                        discard_record();
                        return rhi_error_from_graphics(render_finished.error());
                    }
                    record.render_finished_semaphores.push_back(std::move(*render_finished));
                }

                const u32 frames_in_flight_count =
                    desc.frames_in_flight != 0 ? desc.frames_in_flight : std::max<u32>(1, image_count);
                record.image_available_semaphores.reserve(frames_in_flight_count);
                for (u32 i = 0; i < frames_in_flight_count; ++i) {
                    auto image_available = VulkanSemaphore::create_binary(logical_device_->vk_handle());
                    if (!image_available) {
                        discard_record();
                        return rhi_error_from_graphics(image_available.error());
                    }
                    record.image_available_semaphores.push_back(std::move(*image_available));
                }


                record.composition_sync_interval = (resolution.effective_mode == rhi::PresentMode::Immediate ||
                                                    resolution.effective_mode == rhi::PresentMode::Mailbox)
                                                       ? 0u
                                                       : 1u;

                return swapchains_.insert(std::move(record));
            }


            Foundation::log_info(
                "Composition present unavailable ({}); falling back to vkQueuePresentKHR.",
                composition.error().message);
        }


        const std::array<u32, 2> concurrent_queue_families{
            graphics_queue_->family_index(),
            compute_queue_ != nullptr ? compute_queue_->family_index() : graphics_queue_->family_index(),
        };


        std::unique_ptr<FullScreenExclusiveRequest> full_screen_exclusive_request;
        if (desc.request_full_screen_exclusive && enabled_features_.has(rhi::Feature::FullScreenExclusive)) {
            full_screen_exclusive_request = build_full_screen_exclusive_request(GraphicsPlatform::NativeSurfaceHandle{
                .system = ::SFT::Core::to_graphics_platform(surface->desc.system),
                .display = surface->desc.display,
                .window = surface->desc.window,
                .label = surface->desc.label != nullptr ? std::string_view{surface->desc.label} : std::string_view{},
            });
        }

        VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext = full_screen_exclusive_request ? full_screen_exclusive_request->pnext() : nullptr,
            .surface = surface->surface,
            .minImageCount = choose_image_count(*caps, desc.image_count),
            .imageFormat = format.format,
            .imageColorSpace = format.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = usage,
            .imageSharingMode = present_via_compute ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = present_via_compute ? static_cast<u32>(concurrent_queue_families.size()) : 0,
            .pQueueFamilyIndices = present_via_compute ? concurrent_queue_families.data() : nullptr,
            .preTransform = caps->currentTransform,
            .compositeAlpha = composite_alpha_to_vk(effective_composite_alpha),
            .presentMode = present_mode_to_vk(resolution.effective_mode),
            .clipped = desc.clipped ? VK_TRUE : VK_FALSE,
            .oldSwapchain = old_record != nullptr ? old_record->swapchain.vk_handle() : VK_NULL_HANDLE,
        };

        auto swapchain = VulkanSwapchain::create(logical_device_->vk_handle(), info);
        if (!swapchain && resolution.effective_mode != rhi::PresentMode::Fifo) {


            Foundation::log_warn(
                "Swapchain creation with present mode {} failed; retrying with the guaranteed-available Fifo.",
                rhi::present_mode_name(resolution.effective_mode));
            info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            swapchain = VulkanSwapchain::create(logical_device_->vk_handle(), info);
            if (swapchain) {
                resolution.effective_mode = rhi::PresentMode::Fifo;
                resolution.degraded = true;
            }
        }
        if (!swapchain) {
            return rhi_error_from_graphics(swapchain.error());
        }


        bool full_screen_exclusive_active = false;
        if (full_screen_exclusive_request) {
            if (auto acquired = acquire_full_screen_exclusive_mode(logical_device_->vk_handle(), swapchain->vk_handle());
                acquired) {
                full_screen_exclusive_active = true;
                Foundation::log_info("Swapchain acquired VK_EXT_full_screen_exclusive exclusive mode.");
            } else {
                Foundation::log_info(
                    "Exclusive fullscreen was requested but could not be acquired ({}); presenting normally "
                    "(still fullscreen, still through the compositor) instead.",
                    acquired.error().message);
            }
        }
        resolution.full_screen_exclusive_active = full_screen_exclusive_active;


        VkHdrMetadataEXT initial_hdr_metadata{};
        bool initial_hdr_metadata_set = false;
        if (selected_format->color_space == rhi::ColorSpace::Hdr10St2084 && hdr_metadata_enabled_ && vkSetHdrMetadataEXT != nullptr) {
            const rhi::SurfaceHdrCapabilityQuery hdr_query = ::SFT::Core::query_platform_hdr_display_capabilities(surface->desc);
            if (!hdr_query || !hdr_query.capabilities.display_metadata.has_value()) {
                Foundation::log_info(
                    "HDR metadata: no real display metadata available for this surface ({}); using conservative "
                    "default primaries/luminance instead.",
                    hdr_query.message.message.empty() ? "platform did not report any" : hdr_query.message.message);
            }
            initial_hdr_metadata = build_hdr_metadata(hdr_query);
            const VkSwapchainKHR swapchain_handle = swapchain->vk_handle();
            vkSetHdrMetadataEXT(logical_device_->vk_handle(), 1, &swapchain_handle, &initial_hdr_metadata);
            initial_hdr_metadata_set = true;
        }

        SwapchainRecord record{};
        record.swapchain = std::move(*swapchain);
        record.surface = desc.surface;


        record.presentation_resolution = resolution;
        record.present_via_compute = present_via_compute;
        record.full_screen_exclusive_active = full_screen_exclusive_active;


        record.stored_hdr_metadata = initial_hdr_metadata;
        record.has_hdr_metadata = initial_hdr_metadata_set;
        record.textures.reserve(record.swapchain.image_count());
        record.views.reserve(record.swapchain.image_count());
        record.render_finished_semaphores.reserve(record.swapchain.image_count());


        record.image_available_signal_indices.resize(record.swapchain.image_count(), 0);


        const u32 frames_in_flight_count = desc.frames_in_flight != 0
            ? desc.frames_in_flight
            : std::max<u32>(1, record.swapchain.image_count());
        record.image_available_semaphores.reserve(frames_in_flight_count);
        for (u32 i = 0; i < frames_in_flight_count; ++i) {
            auto image_available = VulkanSemaphore::create_binary(logical_device_->vk_handle());
            if (!image_available) {
                return rhi_error_from_graphics(image_available.error());
            }
            record.image_available_semaphores.push_back(std::move(*image_available));
        }

        for (VkImage image : record.swapchain.images()) {
            VulkanImage borrowed = VulkanImage::borrow(logical_device_->vk_handle(), image, record.swapchain.format(),
                                                       VkExtent3D{extent.width, extent.height, 1}, usage);
            rhi::TextureHandle texture = textures_.insert(TextureRecord{std::move(borrowed), selected_format->format});
            record.textures.push_back(texture);

            const VkImageViewCreateInfo view_info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = record.swapchain.format(),
                .components = {
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY,
                },
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };
            auto view = VulkanImageView::create(logical_device_->vk_handle(), view_info);
            if (!view) {
                return rhi_error_from_graphics(view.error());
            }
            record.views.push_back(texture_views_.insert(std::move(*view)));

            auto render_finished = VulkanSemaphore::create_binary(logical_device_->vk_handle());
            if (!render_finished) {
                return rhi_error_from_graphics(render_finished.error());
            }
            record.render_finished_semaphores.push_back(std::move(*render_finished));
        }

        return swapchains_.insert(std::move(record));
    }

    /// Queries HDR capabilities from the active backend or runtime state.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> VulkanRhiDeviceBridge::query_hdr_capabilities(
        rhi::SurfaceHandle handle) const {
        ZoneScopedN("VulkanRhiDeviceBridge::query_hdr_capabilities");
        const SurfaceRecord *surface = surfaces_.find(handle);
        if (surface == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "query_hdr_capabilities: unknown surface handle.");
        }
        return ::SFT::Core::query_platform_hdr_display_capabilities(surface->desc);
    }

    /// Updates HDR content light level from the supplied values.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param update `update` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`, `RhiErrorCode::Unsupported`.
    rhi::RhiResult VulkanRhiDeviceBridge::update_hdr_content_light_level(
        rhi::SwapchainHandle handle, const rhi::HdrContentLightLevelUpdate &update) {
        ZoneScopedN("VulkanRhiDeviceBridge::update_hdr_content_light_level");
        SwapchainRecord *record = swapchains_.find(handle);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "update_hdr_content_light_level: unknown swapchain handle.");
        }
        if (!record->has_hdr_metadata || vkSetHdrMetadataEXT == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "update_hdr_content_light_level: this swapchain has no HDR metadata to update "
                                  "(only an Hdr10St2084 swapchain created with VK_EXT_hdr_metadata enabled has any).");
        }
        record->stored_hdr_metadata.maxContentLightLevel = update.max_content_light_level_nits;
        record->stored_hdr_metadata.maxFrameAverageLightLevel = update.max_frame_average_light_level_nits;
        const VkSwapchainKHR swapchain_handle = record->swapchain.vk_handle();
        vkSetHdrMetadataEXT(logical_device_->vk_handle(), 1, &swapchain_handle, &record->stored_hdr_metadata);
        return {};
    }

    /// Destroys the swapchain identified by the supplied parameters.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::destroy_swapchain(rhi::SwapchainHandle handle) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::destroy_swapchain");
        SwapchainRecord *record = swapchains_.find(handle);
        if (record != nullptr) {
            for (rhi::TextureViewHandle view : record->views) {
                texture_views_.erase(view);
            }
            for (rhi::TextureHandle texture : record->textures) {
                textures_.erase(texture);
            }


            if (record->owns_composition_resources) {
                destroy_composition_swapchain_resources(logical_device_->vk_handle(), record->composition);
            }


            if (record->full_screen_exclusive_active) {
                release_full_screen_exclusive_mode(logical_device_->vk_handle(), record->swapchain.vk_handle());
            }
        }
        swapchains_.erase(handle);
    }

    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param handle Handle identifying the target object or resource.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    rhi::PresentationResolution VulkanRhiDeviceBridge::presentation_resolution(rhi::SwapchainHandle handle) const noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::presentation_resolution");
        const SwapchainRecord *record = swapchains_.find(handle);
        return record != nullptr ? record->presentation_resolution : rhi::PresentationResolution{};
    }

    /// Acquires next texture.
    ///
    /// @param handle Handle identifying the target object or resource.
    /// @param frame_slot_index Zero-based index of the target element or entry.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`, `RhiErrorCode::OperationFailed`, `RhiErrorCode::DeviceLost`, `RhiErrorCode::SurfaceLost`, `RhiErrorCode::NotReady`.
    rhi::RhiExpected<rhi::SurfaceTexture> VulkanRhiDeviceBridge::acquire_next_texture(rhi::SwapchainHandle handle, u32 frame_slot_index) {
        ZoneScopedN("VulkanRhiDeviceBridge::acquire_next_texture");
        SwapchainRecord *record = swapchains_.find(handle);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "acquire_next_texture: unknown swapchain handle.");
        }
        if (record->image_available_semaphores.empty()) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "acquire_next_texture: swapchain has no image-available semaphores.");
        }
        if (frame_slot_index >= record->image_available_semaphores.size()) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "acquire_next_texture: frame_slot_index is out of range for this swapchain's frame ring.");
        }


        const u32 semaphore_index = frame_slot_index;

        if (record->is_composition_present()) {
            GraphicsPlatform::QueryResult<GraphicsPlatform::CompositionAcquisition> acquisition =
                record->composition.presenter->acquire_next_image();
            if (!acquisition) {
                return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                      string("acquire_next_texture: composition presenter acquire failed: ") +
                                          acquisition.message.message);
            }
            const u32 image_index = acquisition.value.image_index;
            if (image_index >= record->textures.size()) {
                return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                      "acquire_next_texture: composition presenter returned an out-of-range "
                                      "image index.");
            }


            const VkSemaphoreSubmitInfo wait_info = record->composition.present_complete_semaphore.submit_info(
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, acquisition.value.wait_fence_value);
            const VkSemaphoreSubmitInfo signal_info =
                record->image_available_semaphores[semaphore_index].submit_info(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            const VkSubmitInfo2 bridge_submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .waitSemaphoreInfoCount = acquisition.value.wait_fence_value != 0 ? 1u : 0u,
                .pWaitSemaphoreInfos = &wait_info,
                .signalSemaphoreInfoCount = 1,
                .pSignalSemaphoreInfos = &signal_info,
            };
            auto submitted = graphics_queue_->submit(span<const VkSubmitInfo2>(&bridge_submit, 1));
            if (!submitted) {
                return rhi_error_from_graphics(submitted.error());
            }

            record->image_available_signal_indices[image_index] = semaphore_index;
            record->current_image = image_index;
            record->current_suboptimal = false;
            return rhi::SurfaceTexture{
                .swapchain = handle,
                .texture = record->textures[image_index],
                .view = record->views[image_index],
                .image_index = image_index,
                .suboptimal = false,
                .composition_present = true,
            };
        }

        VkAcquireNextImageInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
            .swapchain = record->swapchain.vk_handle(),
            .timeout = 1'000'000ull,
            .semaphore = record->image_available_semaphores[semaphore_index].vk_handle(),
            .fence = VK_NULL_HANDLE,
            .deviceMask = 1,
        };
        u32 image_index = 0;
        const VkResult result = vkAcquireNextImage2KHR(logical_device_->vk_handle(), &info, &image_index);
        if (result == VK_ERROR_DEVICE_LOST) {
            return rhi::rhi_error(rhi::RhiErrorCode::DeviceLost, "acquire_next_texture: vkAcquireNextImage2KHR reported device loss.");
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR) {
            return rhi::rhi_error(rhi::RhiErrorCode::SurfaceLost, "acquire_next_texture: swapchain surface is out of date or lost.");
        }
        if (result == VK_NOT_READY || result == VK_TIMEOUT) {
            return rhi::rhi_error(rhi::RhiErrorCode::NotReady,
                                  string("acquire_next_texture: no swapchain image was ready before the acquire timeout (") +
                                      vk_result_name(result) + ").");
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  string("acquire_next_texture: vkAcquireNextImage2KHR failed with ") +
                                      vk_result_name(result) + " (" + std::to_string(static_cast<int>(result)) + ").");
        }

        record->image_available_signal_indices[image_index] = semaphore_index;
        record->current_image = image_index;
        record->current_suboptimal = result == VK_SUBOPTIMAL_KHR;
        return rhi::SurfaceTexture{
            .swapchain = handle,
            .texture = record->textures[image_index],
            .view = record->views[image_index],
            .image_index = image_index,
            .suboptimal = record->current_suboptimal,
        };
    }

    /// Presents the completed frame to the target surface or swapchain.
    ///
    /// @param desc Description of the resource or operation to perform.
    /// @param queue_lock_wait_ms Queue used or affected by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`, `RhiErrorCode::InvalidArgument`, `RhiErrorCode::Unsupported`.
    rhi::RhiExpected<rhi::PresentOutcome> VulkanRhiDeviceBridge::present(const rhi::PresentDesc &desc, f64 *queue_lock_wait_ms) {
        ZoneScopedN("VulkanRhiDeviceBridge::present");
        if (present_queue_ == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "Vulkan RHI bridge cannot run present: device resources are not ready.");
        }
        SwapchainRecord *record = swapchains_.find(desc.texture.swapchain);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "present: unknown swapchain handle.");
        }
        if (desc.texture.image_index >= record->render_finished_semaphores.size()) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "present: image index is out of range.");
        }

        if (record->is_composition_present()) {
            if (desc.completion_fence) {


                return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                      "present: completion fences are not supported for a composition-present "
                                      "swapchain.");
            }
            const u32 image_index = desc.texture.image_index;
            const VkSemaphoreSubmitInfo wait_info =
                record->render_finished_semaphores[image_index].submit_info(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            ++record->composition.render_complete_value;
            const VkSemaphoreSubmitInfo signal_info = record->composition.render_complete_semaphore.submit_info(
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, record->composition.render_complete_value);
            const VkSubmitInfo2 bridge_submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .waitSemaphoreInfoCount = 1,
                .pWaitSemaphoreInfos = &wait_info,
                .signalSemaphoreInfoCount = 1,
                .pSignalSemaphoreInfos = &signal_info,
            };
            auto submitted = graphics_queue_->submit(span<const VkSubmitInfo2>(&bridge_submit, 1));
            if (!submitted) {
                return rhi_error_from_graphics(submitted.error());
            }
            if (queue_lock_wait_ms != nullptr) {


                *queue_lock_wait_ms = 0.0;
            }

            GraphicsPlatform::QueryMessage present_message = record->composition.presenter->present(
                image_index, record->composition.render_complete_value, record->composition_sync_interval);
            if (!present_message) {
                return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                      string("present: composition presenter present failed: ") +
                                          present_message.message);
            }


            return desc.texture.suboptimal ? rhi::PresentOutcome::Suboptimal : rhi::PresentOutcome::Success;
        }

        const VkSwapchainKHR swapchain = record->swapchain.vk_handle();
        const u32 image_index = desc.texture.image_index;
        const VkSemaphore wait = record->render_finished_semaphores[image_index].vk_handle();
        VkSwapchainPresentFenceInfoKHR completion_info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
            .pNext = nullptr,
            .swapchainCount = 1,
            .pFences = nullptr,
        };
        VkFence completion_fence = VK_NULL_HANDLE;
        if (desc.completion_fence) {
            if (!enabled_features_.has(rhi::Feature::SwapchainMaintenance)) {
                return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                      "present: completion fences require swapchain-maintenance support.");
            }
            VulkanFence *fence = fences_.find(desc.completion_fence);
            if (fence == nullptr) {
                return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                      "present: completion_fence is not a live RHI fence.");
            }
            completion_fence = fence->vk_handle();
            completion_info.pFences = &completion_fence;
        }
        const VkPresentInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = desc.completion_fence ? &completion_info : nullptr,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &wait,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
        };


        VulkanQueue &present_queue =
            (record->present_via_compute && compute_queue_ != nullptr) ? *compute_queue_ : *present_queue_;
        auto result = present_queue.present(info, queue_lock_wait_ms);
        if (!result) {
            return rhi_error_from_graphics(result.error());
        }


        switch (*result) {
            case PresentOutcome::OutOfDate:
                return rhi::PresentOutcome::OutOfDate;
            case PresentOutcome::Suboptimal:
                return rhi::PresentOutcome::Suboptimal;
            case PresentOutcome::Success:
                return desc.texture.suboptimal ? rhi::PresentOutcome::Suboptimal : rhi::PresentOutcome::Success;
        }
        return rhi::PresentOutcome::Success;
    }

} // namespace SFT::Core::Vulkan
