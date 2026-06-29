module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <vector>
#include "volk.h"
// SDL3 and GLFW surface helpers — included after volk so VkInstance/VkSurfaceKHR are already
// defined. GLFW gates glfwCreateWindowSurface behind #if defined(VK_VERSION_1_0) which volk sets;
// we don't define GLFW_INCLUDE_VULKAN to avoid a double-include of vulkan.h.
#include <SDL3/SDL_vulkan.h>
#include <GLFW/glfw3.h>

export module Sturdy.Core:VulkanBackendImpl;

import :VulkanBackend;
import :VulkanDevice;
import :VulkanSurface;
import :VulkanPhysicalDevice;
import :VulkanHelpers;
import :RendererError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;
import Sturdy.Platform;

using std::bad_alloc;
using std::format;
using std::numeric_limits;
using std::unexpected;
using std::unique_ptr;
using std::vector;
using SFT::Platform::Windowing::NativeWindowSystem;
using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowBackendKind;

namespace SFT::Core::Vulkan {

    VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
        void *pUserData
    ) {
        (void)type;
        (void)pUserData;
        switch (severity) {
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
                Foundation::log_debug("[VULKAN API]: {}", pCallbackData->pMessage); break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
                Foundation::log_trace("[VULKAN API]: {}", pCallbackData->pMessage); break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
                Foundation::log_warn("[VULKAN API]: {}", pCallbackData->pMessage); break;
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
            case VK_DEBUG_UTILS_MESSAGE_SEVERITY_FLAG_BITS_MAX_ENUM_EXT:
                Foundation::log_error("[VULKAN API]: {}", pCallbackData->pMessage); break;
        }
        return VK_FALSE;
    }

    namespace {

        [[nodiscard]] u32 sanitize_frames_in_flight(u32 requested) noexcept {
            return requested == 0 ? 2u : requested;
        }

        [[nodiscard]] u32 next_generation(u32 current) noexcept {
            ++current;
            return current == 0 ? 1u : current;
        }

    } // namespace

    VulkanBackend::VulkanBackend(ConstructorKey key)
        : EngineBackend(key) {}

    VulkanBackend::~VulkanBackend() {
        // RAII teardown: drain all in-flight work first, then release GPU objects in
        // reverse creation order (surfaces → swapchains → device → allocator → instance).
        wait_idle();
        destroy_all_surfaces();
        if (vulkan_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(vulkan_instance, nullptr);
        }
        volkFinalize();
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initialize(const RendererCreateInfo &init) {
        create_info_ = init;
        window_ = init.window;

        if (!window_) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "Vulkan backend requires a window to create its primary surface."});
        }

        // Mark initialized before initVulkan so it can create the backend-owned primary surface
        // during bring-up (the surface is needed to query present support). Reset on failure.
        initialized_ = true;

        auto primary_surface = this->initVulkan(init);
        if (!primary_surface.has_value()) [[unlikely]] {
            initialized_ = false;
            return renderer_error(primary_surface.error().code,
                                  format("Initializing Vulkan has failed: {}", primary_surface.error().message));
        }

        return *primary_surface;
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initVulkan(const RendererCreateInfo &init) {
        if (auto result = this->createVulkanInstance(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create Vulkan instance: {}", result.error().message));
        }

        auto surface_info = surface_create_info_from_window(*this->window_, init.features.desired_frames_in_flight);
        if (!surface_info) [[unlikely]] {
            return unexpected(surface_info.error());
        }

        auto surface = createSurface(*surface_info);
        if (!surface) [[unlikely]] {
            return unexpected(surface.error());
        }

        if (auto result = this->findPhysicalDevice(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to find physical GPU: {}", result.error().message));
        }

        if (auto result = this->discoverGraphicsQueue(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to discover a valid graphics queue: {}", result.error().message));
        }

        if (auto result = this->createDevice(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create logical device: {}", result.error().message));
        }

        if (auto result = this->initializeVMA(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to initialize VMA allocator: {}", result.error().message));
        }

        return surface;
    }

    RendererResult VulkanBackend::createVulkanInstance(const RendererCreateInfo &init) {
        (void)init;
        if (auto res = volkInitialize(); res != VK_SUCCESS) {
            return renderer_error(RendererErrorCode::OperationFailed, "Volk failed to initialize");
        }

        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
        VkApplicationInfo appInfo{
            .sType      = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "SturdyEngine Application",
            .apiVersion = VK_API_VERSION_1_4,
        };

        auto extension_res = window_->required_vulkan_instance_extensions();
        if (!extension_res) [[unlikely]] {
            return renderer_error(RendererErrorCode::OperationFailed,
                                  "Failed to get Window extensions list for Vulkan");
        }
        vector<const char *> extensions = extension_res.value();
        vector<const char *> requestedLayers{};

        #ifdef DEBUG
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            requestedLayers.push_back("VK_LAYER_KHRONOS_validation");
            const auto severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
        #else
            const auto severity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        #endif

        VkDebugUtilsMessengerCreateInfoEXT debugInfo{
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = severity,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debugCallback,
        };

        VkInstanceCreateInfo instCreateInfo{
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = &debugInfo,
            .pApplicationInfo        = &appInfo,
            .enabledLayerCount       = static_cast<u32>(requestedLayers.size()),
            .ppEnabledLayerNames     = requestedLayers.data(),
            .enabledExtensionCount   = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        #pragma clang diagnostic pop

        if (vkCreateInstance(&instCreateInfo, nullptr, &this->vulkan_instance) != VK_SUCCESS) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed, "vkCreateInstance failed");
        }

        volkLoadInstance(this->vulkan_instance);
        Foundation::log_info("Vulkan Instance Created...");
        return {};
    }

    RendererResult VulkanBackend::findPhysicalDevice(const RendererCreateInfo &init) {
        (void)init;
        u32 device_count = 0;
        vkEnumeratePhysicalDevices(vulkan_instance, &device_count, nullptr);
        if (!device_count) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                                  "No Vulkan-capable GPUs found on this system.");
        }

        vector<VkPhysicalDevice> raw_devices(device_count);
        vkEnumeratePhysicalDevices(vulkan_instance, &device_count, raw_devices.data());

        f64 max_score = numeric_limits<f64>::lowest();
        VkPhysicalDevice best_raw = VK_NULL_HANDLE;

        for (auto raw : raw_devices) {
            VulkanPhysicalDevice candidate(raw);
            f64 s = candidate.score();
            Foundation::log_info("Found GPU: {} ({}) ID={} score={:.1f}",
                candidate.name(), candidate.type_name(), candidate.properties().deviceID, s);
            if (s > max_score) {
                max_score = s;
                best_raw  = raw;
            }
        }

        physical_device_ = VulkanPhysicalDevice(best_raw);
        Foundation::log_info("Selected GPU: {}", physical_device_.name());
        return {};
    }

    RendererResult VulkanBackend::discoverGraphicsQueue(const RendererCreateInfo &init) {
        (void)init;
        if (auto res = this->physical_device_.findGraphicsQueue(this->surfaces_[0].vk_handle()); !res.has_value()) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed, "Your GPU is apparently not Vulkan Compliant!! the Vulkan spec guarantees one graphics queue and we found zero");
        }
        Foundation::log_info("Successfully got a graphics queue from the physical device!");
        return {};
    }

    RendererResult VulkanBackend::createDevice(const RendererCreateInfo &init) {
        (void)init;

        // Query which features the physical device actually supports.
        VkPhysicalDeviceVulkan14Features supportedFeatures14
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = nullptr };
        VkPhysicalDeviceVulkan13Features supportedFeatures13
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supportedFeatures14 };
        VkPhysicalDeviceVulkan12Features supportedFeatures12
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supportedFeatures13 };
        VkPhysicalDeviceFeatures2 supportedFeatures
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supportedFeatures12 };
        vkGetPhysicalDeviceFeatures2(this->physical_device_.vk_handle(), &supportedFeatures);

        if (not supportedFeatures13.dynamicRendering
         or not supportedFeatures13.synchronization2
         or not supportedFeatures12.timelineSemaphore) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                "Required Vulkan features missing: dynamicRendering, synchronization2, and timelineSemaphore are all required.");
        }

        // Build the enable chain — only request what we verified above.
        VkPhysicalDeviceVulkan14Features features14
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = nullptr };
        VkPhysicalDeviceVulkan13Features features13{
            .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext            = &features14,
            .synchronization2 = VK_TRUE,
            .dynamicRendering = VK_TRUE,
        };
        VkPhysicalDeviceVulkan12Features features12{
            .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext             = &features13,
            .timelineSemaphore = VK_TRUE,
        };
        VkPhysicalDeviceFeatures2 features
            { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12 };

        // Discover queue families. Graphics was already verified by discoverGraphicsQueue;
        // present may share the same index — VulkanDevice::create() deduplicates automatically.
        VkSurfaceKHR surface       = this->surfaces_[0].vk_handle();
        auto         gfx_family     = this->physical_device_.findGraphicsQueue(surface);
        auto         present_family = this->physical_device_.find_present_queue_family(surface);

        // Extensions: swapchain (required for presentation) + calibrated timestamps
        // (Vulkan 1.4 core, needed for anchoring GPU timer to wall clock).
        vector<const char*> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
        };

        VulkanDevice::DeviceCreateDesc desc{
            .graphics_queue_family = gfx_family,
            .present_queue_family  = present_family,
            .extensions            = extensions,
            .features_pnext        = &features,
        };

        auto device_result = VulkanDevice::create(this->physical_device_.vk_handle(), desc);
        if (!device_result.has_value()) [[unlikely]] {
            return renderer_error(device_result.error().code,
                format("VulkanDevice::create failed: {}", device_result.error().message));
        }

        this->logical_device = std::move(*device_result);
        Foundation::log_info("Logical device created on: {}", this->physical_device_.name());
        return {};
    }

    RendererResult VulkanBackend::initializeVMA(const RendererCreateInfo &init) {
        (void)init;
        // TODO(renderer): create a VmaAllocator bound to the VkInstance, VkPhysicalDevice,
        // and VkDevice. Use VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT for BDA support.
        return {};
    }

    RendererExpected<VulkanBackend::SurfaceCreateInfo>
    VulkanBackend::surface_create_info_from_window(Window &window, u32 desired_frames_in_flight) const {
        const auto native = window.native_window_handle();
        if (!native) [[unlikely]] {
            return unexpected(RendererError{
                RendererErrorCode::InitializationFailed,
                format("Failed to query native window handle for Vulkan surface: {}", native.error().message),
            });
        }

        const auto provider_window = window.native_backend_handle();
        if (!provider_window) [[unlikely]] {
            return unexpected(RendererError{
                RendererErrorCode::InitializationFailed,
                format("Failed to query native backend handle for Vulkan surface: {}", provider_window.error().message),
            });
        }

        const auto framebuffer = window.framebuffer_size();
        if (!framebuffer) [[unlikely]] {
            return unexpected(RendererError{
                RendererErrorCode::InitializationFailed,
                format("Failed to query framebuffer size for Vulkan surface: {}", framebuffer.error().message),
            });
        }

        SurfaceCreateInfo info{};
        info.window                  = &window;
        info.descriptor.provider     = to_surface_provider(window.backend_kind());
        info.descriptor.system       = to_surface_system(native->system);
        info.descriptor.display      = native->display;
        info.descriptor.window       = native->window;
        info.descriptor.provider_window = *provider_window;
        info.framebuffer_extent      = {framebuffer->x, framebuffer->y};
        info.desired_frames_in_flight = sanitize_frames_in_flight(desired_frames_in_flight);
        return info;
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::createSurface(const SurfaceCreateInfo &init) {
        if (!initialized_) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "Vulkan backend must be initialized before creating its owned surface."});
        }
        if (!init.window) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "Vulkan surface creation requires a live window."});
        }

        // Find a free slot or grow the vector. Do this before the expensive vkCreate call
        // so OOM from the vector is reported before touching the GPU.
        u32 slot_idx;
        u32 gen;
        bool found_free = false;
        for (usize i = 0; i < surfaces_.size(); ++i) {
            if (!surfaces_[i].is_active()) {
                slot_idx   = static_cast<u32>(i);
                gen        = next_generation(surfaces_[i].generation());
                found_free = true;
                break;
            }
        }
        if (!found_free) {
            slot_idx = static_cast<u32>(surfaces_.size());
            gen      = 1u;
            try {
                surfaces_.emplace_back();
            } catch (const bad_alloc &) {
                return unexpected(RendererError{RendererErrorCode::OutOfMemory,
                                                "Out of memory allocating a Vulkan render surface slot."});
            }
        }

        // Create the platform-specific VkSurfaceKHR.
        VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
        switch (init.descriptor.provider) {
            case SurfaceProvider::SDL3: {
                auto *sdl_window = static_cast<SDL_Window *>(init.descriptor.provider_window);
                if (!SDL_Vulkan_CreateSurface(sdl_window, vulkan_instance, nullptr, &vk_surface)) {
                    return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                                    format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError())});
                }
                break;
            }
            case SurfaceProvider::GLFW: {
                auto *glfw_window = static_cast<GLFWwindow *>(init.descriptor.provider_window);
                if (glfwCreateWindowSurface(vulkan_instance, glfw_window, nullptr, &vk_surface) != VK_SUCCESS) {
                    return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                                    "glfwCreateWindowSurface failed."});
                }
                break;
            }
            default:
                return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                                "Unsupported surface provider; only SDL3 and GLFW are implemented."});
        }

        surfaces_[slot_idx] = VulkanSurface(vk_surface, init.descriptor, init.window,
                                            init.framebuffer_extent,
                                            sanitize_frames_in_flight(init.desired_frames_in_flight), gen);

        Foundation::log_info("Vulkan surface created: provider={} system={} extent={}x{}",
            surface_provider_name(init.descriptor.provider),
            surface_system_name(init.descriptor.system),
            init.framebuffer_extent.width,
            init.framebuffer_extent.height);
        // TODO(renderer): verify the chosen queue family can present to this surface, then
        // create the swapchain and per-frame sync objects once the logical device exists.
        return RenderSurfaceHandle{slot_idx, gen};
    }

    void VulkanBackend::on_surface_resize_needed(RenderSurfaceHandle surface) noexcept {
        VulkanSurface *s = surface_slot(surface);
        if (!s) [[unlikely]] return;
        s->mark_dirty();
        s->refresh_extent();
        // Swapchain rebuild is deferred to the next render_frame call.
        // Resize-to-zero (minimized) is valid — render_frame will skip presentation.
    }

    RendererCapabilities VulkanBackend::capabilities() const noexcept {
        return capabilities_;
    }

    RendererResult VulkanBackend::render_frame(RenderSurfaceHandle surface, const FrameInput &frame) {
        (void)frame;
        VulkanSurface *s = surface_slot(surface);
        if (!s) {
            return renderer_error(RendererErrorCode::SurfaceLost, "Invalid Vulkan render surface handle.");
        }

        if (s->swapchain_dirty()) {
            s->refresh_extent();
        }

        if (s->extent().is_zero()) {
            return {};
        }

        if (s->swapchain_dirty()) {
            // TODO(renderer): wait for this surface's in-flight frames, recreate the swapchain
            // and all swapchain-sized attachments, then clear the flag only on success.
            s->clear_dirty();
        }

        // TODO(renderer): acquire → record (Vulkan 1.4 dynamic rendering) → submit
        // (synchronization2 / vkQueueSubmit2) → present. On VK_ERROR_OUT_OF_DATE_KHR or
        // VK_SUBOPTIMAL_KHR, set swapchain_dirty and retry next frame.
        return {};
    }

    void VulkanBackend::wait_idle() noexcept {
        // TODO(renderer): vkDeviceWaitIdle(device_) once the logical device exists.
    }

    void VulkanBackend::destroy_all_surfaces() noexcept {
        for (usize i = surfaces_.size(); i > 0; --i) {
            VulkanSurface &s = surfaces_[i - 1];
            if (s.is_active()) {
                s.destroy(vulkan_instance);
            }
        }
    }

    VulkanSurface *VulkanBackend::surface_slot(RenderSurfaceHandle handle) noexcept {
        if (!handle.is_valid() || static_cast<usize>(handle.index) >= surfaces_.size()) {
            return nullptr;
        }
        VulkanSurface &s = surfaces_[static_cast<usize>(handle.index)];
        return (s.is_active() && s.generation() == handle.generation) ? &s : nullptr;
    }

    const VulkanSurface *VulkanBackend::surface_slot(RenderSurfaceHandle handle) const noexcept {
        if (!handle.is_valid() || static_cast<usize>(handle.index) >= surfaces_.size()) {
            return nullptr;
        }
        const VulkanSurface &s = surfaces_[static_cast<usize>(handle.index)];
        return (s.is_active() && s.generation() == handle.generation) ? &s : nullptr;
    }

} // namespace SFT::Core::Vulkan

namespace SFT::Core {

    unique_ptr<EngineBackend> create_vulkan_backend() {
        return EngineBackend::create<Vulkan::VulkanBackend>();
    }

} // namespace SFT::Core
