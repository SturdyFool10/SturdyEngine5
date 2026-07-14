// VulkanBackend instance bring-up: volk initialization, VkInstance creation, and the
// validation-layer debug messenger callback (Debug builds only).
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <vector>
#pragma endregion

#include <Foundation/Foundation.hpp>

#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanConstants.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <Platform/Platform.hpp>

using std::format;
using std::string_view;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace {

        // Referenced by address in createVulkanInstance()'s debug messenger create info.
        VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT type,
            const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
            void *pUserData) {
            (void)type;
            (void)pUserData;
            switch (severity) {
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
                case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT:
                    Foundation::log_error("[VULKAN API]: {}", pCallbackData->pMessage);
                    break;
            }
            return VK_FALSE;
        }

    } // namespace

    RendererResult VulkanBackend::createVulkanInstance(const RendererCreateInfo &init) {
        if (auto res = volkInitialize(); res != VK_SUCCESS) {
            return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed, "Volk failed to initialize");
        }
        volk_initialized_ = true;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
        VkApplicationInfo appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "SturdyEngine Application",
            .apiVersion = VULKAN_API_VERSION,
        };

        vector<const char *> extensions = init.wsi_extensions;
        if (extensions.empty()) {
            if (init.window == nullptr) [[unlikely]] {
                return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                      "Vulkan instance creation requires WSI extensions or a live window.");
            }
            auto extension_res = init.window->required_vulkan_instance_extensions();
            if (!extension_res) [[unlikely]] {
                return graphics_backend_error(GraphicsBackendErrorCode::OperationFailed,
                                      "Failed to get Window extensions list for Vulkan");
            }
            extensions = std::move(extension_res.value());
        }
        vector<const char *> requestedLayers{};

        // MoltenVK (and other non-conformant "portability" implementations) are hidden from
        // vkEnumeratePhysicalDevices unless the instance opts in via VK_KHR_portability_enumeration
        // plus VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR. Conformant loaders (Windows/Linux)
        // never advertise this extension, so it's only requested when actually present.
        u32 supported_instance_ext_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &supported_instance_ext_count, nullptr);
        vector<VkExtensionProperties> supported_instance_exts(supported_instance_ext_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &supported_instance_ext_count, supported_instance_exts.data());

        auto add_supported_extension = [&](const char *name) {
            const bool supported = std::ranges::any_of(supported_instance_exts, [name](const VkExtensionProperties &ext) {
                return string_view{ext.extensionName} == string_view{name};
            });
            const bool already_requested = std::ranges::any_of(extensions, [name](const char *requested) {
                return string_view{requested} == string_view{name};
            });
            if (supported && !already_requested) {
                extensions.push_back(name);
            }
            return supported;
        };

        hdr_swapchain_colorspace_enabled_ = false;

        VkInstanceCreateFlags instance_flags = 0;
        if (add_supported_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#if defined(__linux__)
        add_supported_extension("VK_KHR_xlib_surface");
        add_supported_extension("VK_KHR_xcb_surface");
        add_supported_extension("VK_KHR_wayland_surface");
#endif

        if (static_cast<bool>(init.features.presentation.hdr_enabled)) {
            hdr_swapchain_colorspace_enabled_ = add_supported_extension(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
            if (!hdr_swapchain_colorspace_enabled_) {
                return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                              format("HDR presentation requires Vulkan instance extension {}.",
                                                     VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME));
            }
        }

#ifdef DEBUG
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        requestedLayers.push_back("VK_LAYER_KHRONOS_validation");
        const auto severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
#else
        const auto severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
#endif

        VkDebugUtilsMessengerCreateInfoEXT debugInfo{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = severity,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };

        VkInstanceCreateInfo instCreateInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = &debugInfo,
            .flags = instance_flags,
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<u32>(requestedLayers.size()),
            .ppEnabledLayerNames = requestedLayers.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
#pragma clang diagnostic pop

        if (vkCreateInstance(&instCreateInfo, nullptr, &this->vulkan_instance) != VK_SUCCESS) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "vkCreateInstance failed");
        }

        volkLoadInstance(this->vulkan_instance);
        Foundation::log_info("Vulkan Instance Created...");
        return {};
    }

} // namespace SFT::Core::Vulkan
