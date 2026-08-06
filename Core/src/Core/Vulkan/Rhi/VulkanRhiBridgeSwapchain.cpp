// RHI surface/swapchain/presentation implementation for Vulkan.
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#if defined(__linux__)
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
// Xlib.h's `typedef XID Window;` collides with SFT::Platform::Windowing::Window (brought into
// this TU unqualified via Core/Renderer.hpp's `using ...::Window;`) — nothing in this file names
// X11's Window type directly (surface creation goes through reinterpret_cast/static_cast on the
// raw handle), so rename it out of the way for every header that spells the bare word "Window",
// including vulkan_xlib.h below, then undef once both are done with it.
#define Window X11Window
#include <X11/Xlib.h>
#include <wayland-client.h>
#include <xcb/xcb.h>
// X11 headers also define several bare-word macros that collide with this codebase's own
// enumerators (RHI::CompareOp::Always, RHI::BufferUsage::None, Slang's Bool,
// WindowEffectResultKind::Success) — undef every one known to collide, not just the first one
// that happened to bite.
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

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <Core/Vulkan/VulkanSwapchain.hpp>
#include <Core/Vulkan/VulkanSync.hpp>
#include <RHI/RHI.hpp>

using std::optional;
using std::span;
using std::string;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        [[nodiscard]] constexpr VkPresentModeKHR present_mode_to_vk(rhi::PresentMode mode) noexcept {
            switch (mode) {
                case rhi::PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
                case rhi::PresentMode::FifoRelaxed: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
                case rhi::PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
                case rhi::PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
                // The canonical enumerant is the _KHR one (VK_KHR_present_mode_fifo_latest_ready);
                // VK_PRESENT_MODE_FIFO_LATEST_READY_EXT is defined as a plain alias of it in
                // vulkan_core.h, not a distinct value.
                case rhi::PresentMode::FifoLatestReady: return VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        [[nodiscard]] constexpr std::optional<rhi::PresentMode> vk_present_mode_to_rhi(VkPresentModeKHR mode) noexcept {
            switch (mode) {
                case VK_PRESENT_MODE_FIFO_KHR: return rhi::PresentMode::Fifo;
                case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return rhi::PresentMode::FifoRelaxed;
                case VK_PRESENT_MODE_MAILBOX_KHR: return rhi::PresentMode::Mailbox;
                case VK_PRESENT_MODE_IMMEDIATE_KHR: return rhi::PresentMode::Immediate;
                case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR: return rhi::PresentMode::FifoLatestReady;
                default: return std::nullopt; // an exotic mode outside RHI::PresentMode's set — not a candidate.
            }
        }

        // Translates the surface's real, freshly-queried present-mode list into RHI candidates —
        // never assume a mode is available because it was available on another GPU/window/monitor/
        // OS/surface. `fifo_latest_ready_enabled` gates FifoLatestReady specifically: the surface
        // query can legitimately report VK_PRESENT_MODE_FIFO_LATEST_READY_KHR as a raw WSI
        // capability even when the *device* hasn't enabled VK_KHR_present_mode_fifo_latest_ready /
        // its presentModeFifoLatestReady feature bit — passing it to vkCreateSwapchainKHR without
        // that feature enabled is invalid per spec, so it's excluded from the candidate list here
        // rather than only caught at swapchain-creation time.
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

        // Resolves `strategy` against the surface's real supported modes, logging the outcome —
        // info when the strategy's own ideal mode was available, warning when the surface forced a
        // degraded fallback (see RHI::PresentationResolution's own doc comment for what "degraded"
        // means). Never silently claims the requested strategy took effect when it didn't.
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
            } else {
                Foundation::log_info("Presentation strategy {} resolved to {}.", rhi::present_strategy_name(strategy),
                                     rhi::present_mode_name(effective));
            }
            return rhi::PresentationResolution{.strategy = strategy, .effective_mode = effective, .degraded = degraded};
        }

        [[nodiscard]] VkColorSpaceKHR color_space_to_vk(rhi::ColorSpace color_space) noexcept {
            switch (color_space) {
                case rhi::ColorSpace::SrgbNonlinear: return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                case rhi::ColorSpace::Hdr10St2084: return VK_COLOR_SPACE_HDR10_ST2084_EXT;
                case rhi::ColorSpace::ScrgbLinear: return VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT;
                case rhi::ColorSpace::Hdr10Hlg: return VK_COLOR_SPACE_HDR10_HLG_EXT;
                // Deprecated in the Vulkan spec itself (no reason given in the API XML) but still the
                // only enumerant that exists for it — see ColorSpace::DolbyVision's own doc comment
                // (Swapchain.hpp) for why this is best-effort plumbing, not certified Dolby Vision.
                case rhi::ColorSpace::DolbyVision: return VK_COLOR_SPACE_DOLBYVISION_EXT;
                case rhi::ColorSpace::AdobeRgbLinear: return VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT;
                case rhi::ColorSpace::AdobeRgbNonlinear: return VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT;
                case rhi::ColorSpace::DisplayP3Linear: return VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT;
                case rhi::ColorSpace::DisplayP3Nonlinear: return VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT;
                case rhi::ColorSpace::Bt2020Linear: return VK_COLOR_SPACE_BT2020_LINEAR_EXT;
            }
            return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        }

        // Every ColorSpace other than the Vulkan-core default (SrgbNonlinear) is only exposed once
        // VK_EXT_swapchain_colorspace is enabled — not just the HDR ones — so this gates all of them
        // uniformly rather than enumerating cases as they're added. When no exact (format, colorSpace)
        // pair is exposed, choose_surface_format below falls back to *any* format sharing the
        // requested color space rather than silently downgrading to SDR.
        [[nodiscard]] bool requires_swapchain_colorspace_extension(rhi::ColorSpace color_space) noexcept {
            return color_space != rhi::ColorSpace::SrgbNonlinear;
        }

        [[nodiscard]] std::optional<VkSurfaceFormatKHR> choose_surface_format(span<const VkSurfaceFormatKHR> formats,
                                                                              rhi::Format requested,
                                                                              rhi::ColorSpace requested_color_space) noexcept {
            const VkFormat preferred = SFT::Core::Vulkan::to_vk(requested);
            const VkColorSpaceKHR preferred_color_space = color_space_to_vk(requested_color_space);
            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.format == preferred && format.colorSpace == preferred_color_space) {
                    return format;
                }
            }
            if (requires_swapchain_colorspace_extension(requested_color_space)) {
                for (const VkSurfaceFormatKHR &format : formats) {
                    if (format.colorSpace == preferred_color_space) {
                        return format;
                    }
                }
                return std::nullopt;
            }
            for (const VkSurfaceFormatKHR &format : formats) {
                if (format.format == preferred) {
                    return format;
                }
            }
            return formats.empty() ? VkSurfaceFormatKHR{.format = preferred, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                   : formats.front();
        }

        [[nodiscard]] u32 choose_image_count(const VkSurfaceCapabilitiesKHR &caps, u32 requested) noexcept {
            u32 count = requested == 0 ? caps.minImageCount + 1 : requested;
            count = std::max(count, caps.minImageCount);
            if (caps.maxImageCount > 0) {
                count = std::min(count, caps.maxImageCount);
            }
            return count;
        }

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

        [[nodiscard]] VkCompositeAlphaFlagBitsKHR choose_composite_alpha(VkCompositeAlphaFlagsKHR supported,
                                                                         rhi::CompositeAlphaMode requested) noexcept {
            const VkCompositeAlphaFlagBitsKHR preferred = composite_alpha_to_vk(requested);
            if ((supported & preferred) != 0) {
                return preferred;
            }
            constexpr VkCompositeAlphaFlagBitsKHR choices[] = {
                VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            };
            for (VkCompositeAlphaFlagBitsKHR choice : choices) {
                if ((supported & choice) != 0) {
                    return choice;
                }
            }
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }

        // Builds the VkHdrMetadataEXT this bridge sends via vkSetHdrMetadataEXT, preferring the
        // display's real, platform-reported primaries/white-point/luminance
        // (rhi::HdrDisplayMetadata — sourced from EDID/OS/window-system, see HdrMetadataSource) over
        // a fixed guess. `hdr_query` is the freshly-queried capability for this exact surface/display
        // (query_platform_hdr_display_capabilities() is called fresh at every create_swapchain(), so
        // this reflects the display the window is on *right now*, not whatever was true at startup —
        // matters for windows that move to a different monitor between swapchain rebuilds). Falls back
        // to conservative DCI-P3-ish primaries and a 1000/0.001 nit range — this bridge's original
        // fixed values — whenever the platform can't report real metadata (display_metadata unset:
        // Linux/X11 without DRM/EDID access, an unsupported OS backend, or the query itself failing).
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

    VulkanRhiDeviceBridge::SurfaceRecord VulkanRhiDeviceBridge::make_surface_record(VkSurfaceKHR surface, bool owns_surface,
                                                                                    const rhi::SurfaceDesc &desc) {
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

    rhi::RhiExpected<rhi::SurfaceHandle> VulkanRhiDeviceBridge::create_surface(const rhi::SurfaceDesc &desc) {
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

        return surfaces_.insert(make_surface_record(surface, /*owns_surface=*/true, desc));
    }

    rhi::RhiExpected<rhi::SurfaceHandle> VulkanRhiDeviceBridge::import_surface(VkSurfaceKHR surface, const rhi::SurfaceDesc &desc) {
        if (surface == VK_NULL_HANDLE) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument,
                                  "import_surface: cannot import a null VkSurfaceKHR.");
        }
        return surfaces_.insert(make_surface_record(surface, /*owns_surface=*/false, desc));
    }

    void VulkanRhiDeviceBridge::destroy_surface(rhi::SurfaceHandle handle) noexcept {
        SurfaceRecord *record = surfaces_.find(handle);
        if (record != nullptr && record->surface != VK_NULL_HANDLE && record->owns_surface) {
            vkDestroySurfaceKHR(instance_, record->surface, nullptr);
        }
        surfaces_.erase(handle);
    }

    rhi::RhiExpected<rhi::SwapchainHandle> VulkanRhiDeviceBridge::create_swapchain(const rhi::SwapchainDesc &desc) {
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

        if (requires_swapchain_colorspace_extension(desc.color_space) && !hdr_swapchain_colorspace_enabled_) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_swapchain: HDR/wide-gamut color-space requested but VK_EXT_swapchain_colorspace was not enabled.");
        }
        const auto selected_format = choose_surface_format(*formats, desc.format, desc.color_space);
        if (!selected_format) {
            return rhi::rhi_error(rhi::RhiErrorCode::Unsupported,
                                  "create_swapchain: surface does not expose a Vulkan surface format for the requested color space.");
        }
        const VkSurfaceFormatKHR format = *selected_format;
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

        // Present-from-compute: only relevant when the device actually has a compute queue and its
        // family differs from graphics' (VulkanBackendDevice.cpp's dedicated-compute-family search
        // explicitly excludes the graphics bit, so whenever a compute queue exists its family is
        // *always* different from graphics' — a same-family compute queue would need none of this,
        // since presenting from a different VkQueue in the same family needs no ownership transfer).
        // desc.allow_present_from_compute is the engine's request (Core::PresentationSettings,
        // opt-out by default); queue_family_supports_present() is the one place that request gets
        // checked against real per-surface support, same "never assume" discipline
        // resolve_present_mode() above already follows for present modes.
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

        // Presenting from a different queue *family* than the one that rendered into the image is a
        // real queue-family-ownership concern for an EXCLUSIVE-sharing-mode swapchain image (the
        // default below) — CONCURRENT sharing across exactly the two families that ever touch this
        // image sidesteps needing an explicit ownership-transfer barrier pair every frame, at the
        // (here negligible — only two queue families, one swapchain image at a time) usual CONCURRENT
        // bandwidth cost. The second family index is never read unless present_via_compute is true
        // (which already implies compute_queue_ != nullptr), so the graphics-family fallback here
        // avoids dereferencing a possibly-null compute_queue_ rather than expressing anything real.
        const std::array<u32, 2> concurrent_queue_families{
            graphics_queue_->family_index(),
            compute_queue_ != nullptr ? compute_queue_->family_index() : graphics_queue_->family_index(),
        };

        VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
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
            .compositeAlpha = choose_composite_alpha(caps->supportedCompositeAlpha, desc.composite_alpha),
            .presentMode = present_mode_to_vk(resolution.effective_mode),
            .clipped = desc.clipped ? VK_TRUE : VK_FALSE,
            .oldSwapchain = old_record != nullptr ? old_record->swapchain.vk_handle() : VK_NULL_HANDLE,
        };

        auto swapchain = VulkanSwapchain::create(logical_device_->vk_handle(), info);
        if (!swapchain && resolution.effective_mode != rhi::PresentMode::Fifo) {
            // Even a mode reported as supported by the query above can fail at creation time if the
            // surface/platform state changed in between (window moved to another monitor, display
            // mode changed, compositor state changed, ...) — retry once with the one present mode
            // every conformant Vulkan implementation is guaranteed to accept, rather than failing
            // the whole swapchain (re)build outright.
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

        // Flip-model composed presentation on Windows (the fast "Composed: Flip" path vs. the
        // legacy blit-copy "Composed: Copy with GPU/CPU" path) needs imageCount >= 2 and an opaque
        // composite alpha; both are already this bridge's default resolution, but log the values
        // actually negotiated so a regression (e.g. a non-opaque composite alpha request) is
        // greppable without reaching for PresentMon just to sanity-check preconditions.
        Foundation::log_info(
            "Swapchain created: imageCount={} compositeAlpha={} presentMode={}",
            info.minImageCount, static_cast<u32>(info.compositeAlpha), rhi::present_mode_name(resolution.effective_mode));

        VkHdrMetadataEXT initial_hdr_metadata{};
        bool initial_hdr_metadata_set = false;
        if (desc.color_space == rhi::ColorSpace::Hdr10St2084 && hdr_metadata_enabled_ && vkSetHdrMetadataEXT != nullptr) {
            const rhi::SurfaceHdrCapabilityQuery hdr_query = rhi::query_platform_hdr_display_capabilities(surface->desc);
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
        // Requested-vs-effective state for diagnostics (see PresentationResolution's own doc
        // comment) — reflects whatever mode the swapchain actually ended up with, including the
        // Fifo-retry-on-creation-failure path above.
        record.presentation_resolution = resolution;
        record.present_via_compute = present_via_compute;
        // Retained so update_hdr_content_light_level() can resend this metadata with just the
        // content-light-level fields overwritten, without re-querying display primaries — see
        // SwapchainRecord::stored_hdr_metadata's own doc comment (VulkanRhiBridge.hpp).
        record.stored_hdr_metadata = initial_hdr_metadata;
        record.has_hdr_metadata = initial_hdr_metadata_set;
        record.textures.reserve(record.swapchain.image_count());
        record.views.reserve(record.swapchain.image_count());
        record.image_available_semaphores.reserve(record.swapchain.image_count());
        record.render_finished_semaphores.reserve(record.swapchain.image_count());
        record.image_available_signal_indices.resize(record.swapchain.image_count(), 0);

        for (VkImage image : record.swapchain.images()) {
            VulkanImage borrowed = VulkanImage::borrow(logical_device_->vk_handle(), image, record.swapchain.format(),
                                                       VkExtent3D{extent.width, extent.height, 1}, usage);
            rhi::TextureHandle texture = textures_.insert(TextureRecord{std::move(borrowed), desc.format});
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

            auto image_available = VulkanSemaphore::create_binary(logical_device_->vk_handle());
            if (!image_available) {
                return rhi_error_from_graphics(image_available.error());
            }
            record.image_available_semaphores.push_back(std::move(*image_available));

            auto render_finished = VulkanSemaphore::create_binary(logical_device_->vk_handle());
            if (!render_finished) {
                return rhi_error_from_graphics(render_finished.error());
            }
            record.render_finished_semaphores.push_back(std::move(*render_finished));
        }

        return swapchains_.insert(std::move(record));
    }

    rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> VulkanRhiDeviceBridge::query_hdr_capabilities(
        rhi::SurfaceHandle handle) const {
        const SurfaceRecord *surface = surfaces_.find(handle);
        if (surface == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "query_hdr_capabilities: unknown surface handle.");
        }
        return rhi::query_platform_hdr_display_capabilities(surface->desc);
    }

    rhi::RhiResult VulkanRhiDeviceBridge::update_hdr_content_light_level(
        rhi::SwapchainHandle handle, const rhi::HdrContentLightLevelUpdate &update) {
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

    void VulkanRhiDeviceBridge::destroy_swapchain(rhi::SwapchainHandle handle) noexcept {
        SwapchainRecord *record = swapchains_.find(handle);
        if (record != nullptr) {
            for (rhi::TextureViewHandle view : record->views) {
                texture_views_.erase(view);
            }
            for (rhi::TextureHandle texture : record->textures) {
                textures_.erase(texture);
            }
        }
        swapchains_.erase(handle);
    }

    rhi::PresentationResolution VulkanRhiDeviceBridge::presentation_resolution(rhi::SwapchainHandle handle) const noexcept {
        const SwapchainRecord *record = swapchains_.find(handle);
        return record != nullptr ? record->presentation_resolution : rhi::PresentationResolution{};
    }

    rhi::RhiExpected<rhi::SurfaceTexture> VulkanRhiDeviceBridge::acquire_next_texture(rhi::SwapchainHandle handle) {
        SwapchainRecord *record = swapchains_.find(handle);
        if (record == nullptr) {
            return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, "acquire_next_texture: unknown swapchain handle.");
        }
        if (record->image_available_semaphores.empty()) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  "acquire_next_texture: swapchain has no image-available semaphores.");
        }

        const u32 semaphore_index = record->acquire_cursor++ % static_cast<u32>(record->image_available_semaphores.size());
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

    rhi::RhiExpected<rhi::PresentOutcome> VulkanRhiDeviceBridge::present(const rhi::PresentDesc &desc, f64 *queue_lock_wait_ms) {
        if (graphics_queue_ == nullptr) {
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

        const VkSwapchainKHR swapchain = record->swapchain.vk_handle();
        const u32 image_index = desc.texture.image_index;
        const VkSemaphore wait = record->render_finished_semaphores[image_index].vk_handle();
        const VkPresentInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &wait,
            .swapchainCount = 1,
            .pSwapchains = &swapchain,
            .pImageIndices = &image_index,
        };
        // record->present_via_compute was decided once at create_swapchain() time against this exact
        // swapchain's surface (RHI::PresentationResolution::present_queue_is_compute's own doc
        // comment) — compute_queue_ is guaranteed non-null whenever it's true.
        VulkanQueue &present_queue = (record->present_via_compute && compute_queue_ != nullptr) ? *compute_queue_ : *graphics_queue_;
        auto result = present_queue.present(info, queue_lock_wait_ms);
        if (!result) {
            return rhi_error_from_graphics(result.error());
        }
        // acquire_next_texture's own staleness signal (desc.texture.suboptimal) folds in here too:
        // if present() itself came back clean but the image was already known-suboptimal at
        // acquisition, the overall outcome must still say so -- Suboptimal, not silently Success.
        // OutOfDate always wins over Suboptimal (it is the more urgent of the two).
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
