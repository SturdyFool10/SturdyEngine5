module;
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <chrono>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
// SDL3 and GLFW surface helpers — included after volk so VkInstance/VkSurfaceKHR are already
// defined. GLFW gates glfwCreateWindowSurface behind #if defined(VK_VERSION_1_0) which volk sets;
// we don't define GLFW_INCLUDE_VULKAN to avoid a double-include of vulkan.h.
#include <GLFW/glfw3.h>
#include <SDL3/SDL_vulkan.h>

export module Sturdy.Core:VulkanBackendImpl;

import :VulkanAllocator;
import :VulkanBackend;
import :VulkanConstants;
import :VulkanDevice;
import :VulkanImage;
import :VulkanQueue;
import :VulkanShaderModule;
import :VulkanSurface;
import :VulkanSwapchain;
import :VulkanSync;
import :VulkanPhysicalDevice;
import :VulkanPipeline;
import :VulkanHelpers;
import :RendererError;
import :Renderer;
import :RenderSurface;
import :Shader;
import :ShaderDiscovery;
import Sturdy.Foundation;
import Sturdy.Platform;

using SFT::Platform::Windowing::NativeWindowSystem;
using SFT::Platform::Windowing::Window;
using SFT::Platform::Windowing::WindowBackendKind;
using SFT::Platform::Windowing::WindowId;
using std::bad_alloc;
using std::format;
using std::make_shared;
using std::numeric_limits;
using std::shared_ptr;
using std::span;
using std::string;
using std::string_view;
using std::unexpected;
using std::unique_ptr;
using std::vector;
using std::chrono::duration;
using std::chrono::steady_clock;

namespace SFT::Core::Vulkan {

    // Used by createVulkanInstance() below as the validation-layer debug messenger callback —
    // defined first since it's referenced by address rather than declared elsewhere.
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

    // Used by surface_create_info_from_window() and createSurface() below.
    namespace {

        [[nodiscard]] u32 sanitize_frames_in_flight(u32 requested) noexcept {
            return requested == 0 ? 2u : requested;
        }

        // Drops a trailing ".slang" so shader module keys are addressed by bare source name, e.g.
        // "Shaders/triangle" rather than "Shaders/triangle.slang". Used both when filing modules and
        // when looking them up, so callers may pass the path with or without the extension.
        [[nodiscard]] string strip_slang_extension(string_view path) {
            constexpr string_view extension = ".slang";
            if (path.size() >= extension.size() && path.substr(path.size() - extension.size()) == extension) {
                path.remove_suffix(extension.size());
            }
            return string{path};
        }

    } // namespace

    VulkanBackend::VulkanBackend(ConstructorKey key)
        : EngineBackend(key) {}

    RendererCapabilities VulkanBackend::capabilities() const noexcept {
        return capabilities_;
    }

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

    void VulkanBackend::wait_idle() noexcept {
        // TODO(renderer): vkDeviceWaitIdle(device_) once the logical device exists.
    }

    void VulkanBackend::destroySwapchain(VulkanSurface &surface) noexcept {
        VulkanSwapchain &swapchain = surface.swapchain();
        const bool had_swapchain_resources = swapchain.is_valid() ||
                                             !swapchain.image_views().empty() ||
                                             swapchain.depth_image().is_valid() ||
                                             swapchain.depth_image_view().is_valid() ||
                                             !swapchain.render_finished_semaphores().empty();

        swapchain.destroy();
        surface.mark_dirty();

        if (had_swapchain_resources) {
            Foundation::log_info("Vulkan swapchain resources destroyed");
        }
    }

    void VulkanBackend::destroySurface(VulkanSurface &surface) noexcept {
        destroySwapchain(surface);
        surface.destroy(vulkan_instance);
    }

    void VulkanBackend::destroy_all_surfaces() noexcept {
        for (auto &[id, s] : surfaces_) {
            destroySurface(s);
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

    VulkanBackend::~VulkanBackend() {
        // RAII teardown: drain all in-flight work first, then release GPU objects in
        // reverse creation order (surfaces → shaders/pipelines/layouts → allocator → device → instance). This must happen
        // explicitly and in this order, since automatic member destruction would otherwise run
        // *after* this body (i.e. after vkDestroyInstance below) and tear the allocator/device
        // down against an already-destroyed instance.
        wait_idle();
        destroy_all_surfaces();
        // Shader modules, sync objects, the graphics pipeline, and its layout hold handles owned by
        // the logical device, so they must be released before the device is destroyed below.
        shader_modules_.clear();
        frameResources_.clear();
        frameTimelineSemaphore.destroy();
        graphicsPipeline.destroy();
        if (pipelinelayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(this->logicalDevice.vk_handle(), pipelinelayout, nullptr);
            pipelinelayout = VK_NULL_HANDLE;
        }
        vmaAllocator.destroy();
        logicalDevice.destroy();
        if (vulkan_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(vulkan_instance, nullptr);
            vulkan_instance = VK_NULL_HANDLE;
        }
        volkFinalize();
    }

    void VulkanBackend::on_surface_resize_needed(RenderSurfaceHandle surface) noexcept {
        VulkanSurface *s = surface_slot(surface);
        if (!s) [[unlikely]]
            return;
        s->mark_dirty();
        s->refresh_extent();
        // Swapchain rebuild is deferred to the next render_frame call.
        // Resize-to-zero (minimized) is valid — render_frame will skip presentation.
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

    RendererResult VulkanBackend::findPhysicalDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
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
                                 candidate.name(),
                                 candidate.type_name(),
                                 candidate.properties().deviceID,
                                 s);
            if (s > max_score) {
                max_score = s;
                best_raw = raw;
            }
        }

        physicalDevice = VulkanPhysicalDevice(best_raw);
        Foundation::log_info("Selected GPU: {}", physicalDevice.name());

        // make sure we support the swapchain format we plan to use
        u32 formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(this->physicalDevice.vk_handle(), primary_surface, &formatCount, nullptr);
        vector<VkSurfaceFormatKHR> surfaceFormats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(this->physicalDevice.vk_handle(), primary_surface, &formatCount, surfaceFormats.data());

        bool formatSupported = false;
        for (const auto &surfaceFormat : surfaceFormats) {
            if (surfaceFormat.format == SWAPCHAIN_FORMAT) {
                formatSupported = true;
                break;
            }
        }

        if (!formatSupported) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed, "Physical Device Selection failed at checking surface formats");
        }

        return {};
    }

    RendererResult VulkanBackend::discoverGraphicsQueue(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        (void)init;
        if (auto res = this->physicalDevice.findGraphicsQueue(primary_surface); !res.has_value()) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed, "Your GPU is apparently not Vulkan Compliant!! the Vulkan spec guarantees one graphics queue and we found zero");
        }
        Foundation::log_info("Successfully got a graphics queue from the physical device!");
        return {};
    }

    RendererResult VulkanBackend::createDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        (void)init;

        // Query which features the physical device actually supports.
        VkPhysicalDeviceVulkan14Features supportedFeatures14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = nullptr};
        VkPhysicalDeviceVulkan13Features supportedFeatures13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supportedFeatures14};
        VkPhysicalDeviceVulkan12Features supportedFeatures12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supportedFeatures13};
        VkPhysicalDeviceFeatures2 supportedFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supportedFeatures12};
        vkGetPhysicalDeviceFeatures2(this->physicalDevice.vk_handle(), &supportedFeatures);

        if (not supportedFeatures13.dynamicRendering or not supportedFeatures13.synchronization2 or not supportedFeatures12.timelineSemaphore) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                                  "Required Vulkan features missing: dynamicRendering, synchronization2, and timelineSemaphore are all required.");
        }

        // Build the enable chain — only request what we verified above.
        VkPhysicalDeviceVulkan14Features features14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = nullptr};
        VkPhysicalDeviceVulkan13Features features13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &features14,
            .synchronization2 = VK_TRUE,
            .dynamicRendering = VK_TRUE,
        };
        VkPhysicalDeviceVulkan12Features features12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &features13,
            .timelineSemaphore = VK_TRUE,
        };
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features12};

        // Discover queue families. Graphics was already verified by discoverGraphicsQueue;
        // present may share the same index — VulkanDevice::create() deduplicates automatically.
        auto gfx_family = this->physicalDevice.findGraphicsQueue(primary_surface);
        auto present_family = this->physicalDevice.find_present_queue_family(primary_surface);

        // Extensions: swapchain (required for presentation) + calibrated timestamps
        // (Vulkan 1.4 core, needed for anchoring GPU timer to wall clock).
        vector<const char *> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
        };

        VulkanDevice::DeviceCreateDesc desc{
            .graphics_queue_family = gfx_family,
            .present_queue_family = present_family,
            .extensions = extensions,
            .features_pnext = &features,
        };

        auto device_result = VulkanDevice::create(this->physicalDevice.vk_handle(), desc);
        if (!device_result.has_value()) [[unlikely]] {
            return renderer_error(device_result.error().code,
                                  format("VulkanDevice::create failed: {}", device_result.error().message));
        }

        this->logicalDevice = std::move(*device_result);
        Foundation::log_info(
            "Logical device created on: {}",
            this->physicalDevice.name());

        // VulkanDevice::create() already retrieved the graphics queue since gfx_family was
        // passed in desc above — pull it out rather than querying vkGetDeviceQueue again.
        auto &device_graphics_queue = this->logicalDevice.graphics_queue();
        if (!device_graphics_queue.has_value()) [[unlikely]] {
            Foundation::log_error("Failed to produce a VkQueue for graphics!");
            return renderer_error(RendererErrorCode::InitializationFailed, "Failed to get a graphics queue for drawing graphics");
        }
        this->gfxQueue = std::move(*device_graphics_queue);
        return {};
    }

    RendererResult VulkanBackend::initializeVMA(const RendererCreateInfo &init) {
        (void)init;

        VulkanAllocator::CreateDesc desc{
            .physical_device = this->physicalDevice.vk_handle(),
            .device = this->logicalDevice.vk_handle(),
            .instance = this->vulkan_instance,
            .api_version = VULKAN_API_VERSION,
        };

        auto allocator_result = VulkanAllocator::create(desc);
        if (!allocator_result.has_value()) [[unlikely]] {
            return renderer_error(allocator_result.error().code,
                                  format("Failed to start VMA: {}", allocator_result.error().message));
        }

        this->vmaAllocator = std::move(*allocator_result);

        Foundation::log_info("VMA Initialization was a success!");
        return {};
    }

    RendererResult VulkanBackend::createSwapchain(const RendererCreateInfo &init, VulkanSurface &surface) {
        (void)init;

        // Swapchain extent must match the surface's pixel dimensions, not the window's
        // logical/point size — these differ under HiDPI scaling.
        auto windowExtentResult = surface.window()->framebuffer_size();
        if (!windowExtentResult.has_value()) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                                  format("Failed to query framebuffer size for swapchain creation: {}", windowExtentResult.error().message));
        }
        auto winSize = windowExtentResult.value();

        // request the apropriate number of images
        VkSurfaceCapabilitiesKHR surfaceCaps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
                this->physicalDevice.vk_handle(),
                surface.vk_handle(),
                &surfaceCaps) != VK_SUCCESS) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                                  "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
        }
        u32 requestedImageCount = sanitize_frames_in_flight(std::max(2u, surfaceCaps.minImageCount));
        if (surfaceCaps.maxImageCount > 0) [[likely]] {
            requestedImageCount = std::min(requestedImageCount, surfaceCaps.maxImageCount);
        }

        VkSwapchainCreateInfoKHR swapchainCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surface.vk_handle(),
            .minImageCount = requestedImageCount,
            .imageFormat = SWAPCHAIN_FORMAT,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
            .imageExtent{.width = winSize.x, .height = winSize.y},
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .preTransform = surfaceCaps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = VK_PRESENT_MODE_FIFO_RELAXED_KHR};

        auto swapchain_result = VulkanSwapchain::create(this->logicalDevice.vk_handle(), swapchainCreateInfo);
        if (!swapchain_result.has_value()) [[unlikely]] {
            return renderer_error(swapchain_result.error().code,
                                  format("Failed to create swapchain: {}", swapchain_result.error().message));
        }

        surface.set_swapchain(std::move(*swapchain_result));
        surface.clear_dirty();
        Foundation::log_info("Vulkan swapchain created: {}x{} images={} format={}",
                             surface.swapchain().extent().width,
                             surface.swapchain().extent().height,
                             surface.swapchain().image_count(),
                             vulkan_format_name(surface.swapchain().format()));

        // One VkImageView per swapchain image — dynamic rendering attachments and any future
        // framebuffer-less render pass need a view, not the raw VkImage.
        vector<VulkanImageView> image_views;
        image_views.reserve(surface.swapchain().image_count());
        for (VkImage image : surface.swapchain().images()) {
            VkImageViewCreateInfo viewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = surface.swapchain().format(),
                .components = {
                    .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                    .a = VK_COMPONENT_SWIZZLE_IDENTITY,
                },
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            };

            auto view_result = VulkanImageView::create(this->logicalDevice.vk_handle(), viewCreateInfo);
            if (!view_result.has_value()) [[unlikely]] {
                return renderer_error(view_result.error().code,
                                      format("Failed to create swapchain image view: {}", view_result.error().message));
            }
            image_views.push_back(std::move(*view_result));
        }

        const u32 view_count = static_cast<u32>(image_views.size());
        surface.swapchain().set_image_views(std::move(image_views));
        Foundation::log_info("Vulkan swapchain image views created: count={}", view_count);

        // One render-finished semaphore per swapchain image, signaled by the submit that
        // renders into that image and waited on by the present that follows it.
        vector<VulkanSemaphore> render_finished_semaphores;
        render_finished_semaphores.reserve(surface.swapchain().image_count());
        for (u32 i = 0; i < surface.swapchain().image_count(); ++i) {
            auto semaphore_result = VulkanSemaphore::create_binary(this->logicalDevice.vk_handle());
            if (!semaphore_result.has_value()) [[unlikely]] {
                return renderer_error(semaphore_result.error().code,
                                      format("Failed to create render-finished semaphore: {}", semaphore_result.error().message));
            }
            render_finished_semaphores.push_back(std::move(*semaphore_result));
        }

        const u32 semaphore_count = static_cast<u32>(render_finished_semaphores.size());
        surface.swapchain().set_render_finished_semaphores(std::move(render_finished_semaphores));
        Foundation::log_info("Vulkan render-finished semaphores created: count={}", semaphore_count);

        VkImageCreateInfo depthCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = DEPTH_FORMAT,
            .extent = {.width = winSize.x, .height = winSize.y, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo depthAllocationInfo{
            .flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
            .usage = VMA_MEMORY_USAGE_AUTO,
        };

        auto depth_image_result = this->vmaAllocator.create_image(
            this->logicalDevice.vk_handle(),
            depthCreateInfo,
            depthAllocationInfo);
        if (!depth_image_result.has_value()) [[unlikely]] {
            return renderer_error(depth_image_result.error().code,
                                  format("Failed to create depth image: {}", depth_image_result.error().message));
        }

        auto depth_image = std::move(*depth_image_result);
        auto depth_view_result = depth_image.create_view(VK_IMAGE_ASPECT_DEPTH_BIT,
                                                         VK_IMAGE_VIEW_TYPE_2D,
                                                         0,
                                                         1,
                                                         0,
                                                         1);
        if (!depth_view_result.has_value()) [[unlikely]] {
            return renderer_error(depth_view_result.error().code,
                                  format("Failed to create depth image view: {}", depth_view_result.error().message));
        }

        surface.swapchain().set_depth_attachment(std::move(depth_image), std::move(*depth_view_result));
        Foundation::log_info("Vulkan depth image created: {}x{} format={}",
                             winSize.x,
                             winSize.y,
                             vulkan_format_name(DEPTH_FORMAT));

        return {};
    }

    RendererResult VulkanBackend::createShaders(const RendererCreateInfo &init) {
        // The backend owns turning the engine's reflected-but-uncompiled shaders into its native
        // format. For Vulkan that means recompiling each source to SPIR-V 1.6 with maximal
        // optimization, then producing one VkShaderModule per entry point. Each module retains a
        // shared handle to its source file's reflection and is filed under (source file, entry point).
        Slang::ShaderCompiler compiler;

        Slang::ShaderCompileOptions options{};
        options.targets = {Slang::ShaderTarget{.format = Slang::ShaderTargetFormat::Spirv, .profile = "spirv_1_6"}};
        options.optimization = Slang::ShaderOptimizationLevel::Maximal;

        const auto total_start = steady_clock::now();

        for (const Slang::UnCompiledShader &uncompiled : init.uncompiled_shaders) {
            const string source_path{uncompiled.source.path};        // real path — for logs/errors
            const string source_file = strip_slang_extension(source_path); // key + stored provenance

            const auto frontend_start = steady_clock::now();
            auto compiled = compiler.compile(uncompiled.source, options);
            const duration<double, std::milli> frontend_elapsed = steady_clock::now() - frontend_start;
            if (!compiled) [[unlikely]] {
                return renderer_error(RendererErrorCode::OperationFailed,
                                      format("Failed to compile shader '{}' to SPIR-V: {}",
                                             source_path, compiled.error().message));
            }

            // One reflection per source file, shared by every entry point's module.
            auto reflection = make_shared<const Slang::ShaderReflection>(compiled->reflection());

            for (usize entry_index = 0; entry_index < reflection->entry_points.size(); ++entry_index) {
                const Slang::ShaderEntryPointReflection &entry = reflection->entry_points[entry_index];

                const VkShaderStageFlagBits stage = to_vk_shader_stage(entry.stage);
                if (stage == 0) [[unlikely]] {
                    return renderer_error(RendererErrorCode::Unsupported,
                                          format("Shader '{}' entry point '{}' has no Vulkan stage mapping.",
                                                 source_path, entry.name));
                }

                // Times the part of the pipeline that produces a shader Vulkan can actually call:
                // target codegen for this entry point plus building its VkShaderModule. The Slang
                // front-end parse/reflect above (frontend_elapsed) is shared across every entry
                // point in the file, so it's reported separately rather than folded in here.
                const auto entry_start = steady_clock::now();

                auto bytecode = compiled->entry_point_code(entry_index, 0);
                if (!bytecode) [[unlikely]] {
                    return renderer_error(RendererErrorCode::OperationFailed,
                                          format("Failed to emit SPIR-V for '{}' entry point '{}': {}",
                                                 source_path, entry.name, bytecode.error().message));
                }

                // SPIR-V is a stream of 32-bit words; the byte count is always a multiple of 4.
                if (bytecode->bytes.size() % sizeof(u32) != 0) [[unlikely]] {
                    return renderer_error(RendererErrorCode::OperationFailed,
                                          format("SPIR-V for '{}' entry point '{}' is not word-aligned.",
                                                 source_path, entry.name));
                }
                const span<const u32> words{
                    reinterpret_cast<const u32 *>(bytecode->bytes.data()),
                    bytecode->bytes.size() / sizeof(u32),
                };

                auto module = VulkanShaderModule::create(
                    this->logicalDevice.vk_handle(),
                    words,
                    source_file,
                    entry.name,
                    stage,
                    reflection);
                if (!module) [[unlikely]] {
                    return renderer_error(module.error().code,
                                          format("Failed to create VkShaderModule for '{}' entry point '{}': {}",
                                                 source_path, entry.name, module.error().message));
                }

                const duration<double, std::milli> entry_elapsed = steady_clock::now() - entry_start;
                Foundation::log_info("Shader '{}' entry point '{}': codegen+module {:.3f} ms (+{:.3f} ms front-end)",
                                     source_path, entry.name, entry_elapsed.count(), frontend_elapsed.count());

                VulkanShaderModuleKey key{source_file, entry.name};
                if (auto [it, inserted] = shader_modules_.try_emplace(std::move(key), std::move(*module)); !inserted) [[unlikely]] {
                    return renderer_error(RendererErrorCode::OperationFailed,
                                          format("Duplicate shader entry point '{}' in '{}'.", entry.name, source_path));
                }
            }

            Foundation::log_info("Compiled shader '{}' to SPIR-V 1.6: {} entry point(s)",
                                 source_path, reflection->entry_points.size());
        }

        const duration<double, std::milli> total_elapsed = steady_clock::now() - total_start;
        Foundation::log_info("Shader compilation complete: {} module(s) from {} source file(s) in {:.3f} ms",
                             shader_modules_.size(), init.uncompiled_shaders.size(), total_elapsed.count());
        return {};
    }

    const VulkanShaderModule *VulkanBackend::find_shader_module(string_view source_file,
                                                               string_view entry_point) const noexcept {
        const auto it = shader_modules_.find(VulkanShaderModuleKey{strip_slang_extension(source_file), string{entry_point}});
        return it != shader_modules_.end() ? &it->second : nullptr;
    }

    RendererResult VulkanBackend::createGraphicsPipeline(const RendererCreateInfo &init) {
        (void)init;
        //define a pipeline layout
        VkPipelineLayoutCreateInfo pipelineCreateInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 0,
            .pushConstantRangeCount = 0
        };

        if (vkCreatePipelineLayout(this->logicalDevice.vk_handle(), &pipelineCreateInfo, nullptr, &pipelinelayout) != VK_SUCCESS) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed, "vkCreatePipelineLayout failed.");
        }

        auto *vert = find_shader_module("Shaders/triangle", "vertexMain");
        auto *frag = find_shader_module("Shaders/triangle", "fragmentMain");
        if (!vert or !frag) [[unlikely]] {
            return renderer_error(RendererErrorCode::InitializationFailed,
                                  "Vulkan graphics pipeline requires 'Shaders/triangle' vertexMain/fragmentMain shader modules.");
        }

        // stage_info() builds the create info from the module's own stage + entry point (stored as
        // an owned std::string, so .pName stays valid), rather than reaching into string_view here.
        const vector<VkPipelineShaderStageCreateInfo> shaderStages{
            vert->stage_info(),
            frag->stage_info(),
        };

        //Vertex Pulling, don't define
        VkPipelineVertexInputStateCreateInfo vertInputInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO
        };

        //input assembly, we'll be drawing triangle lists
        VkPipelineInputAssemblyStateCreateInfo inputAssemblyInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
        };

        //depth/stencil config
        VkPipelineDepthStencilStateCreateInfo stencilInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
            .stencilTestEnable = VK_FALSE
        };

        // Dynamic viewport/scissor state means these values are overwritten at draw time, but keep
        // valid backing pointers here so strict drivers never see nonzero counts with null arrays.
        VkViewport viewport{};
        VkRect2D scissor{};
        VkPipelineViewportStateCreateInfo viewportInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };

        //raster settings
        VkPipelineRasterizationStateCreateInfo rasterizationInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };

        //multisampling settings
        VkPipelineMultisampleStateCreateInfo multisampleInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };

        // Alpha-blending
        VkPipelineColorBlendAttachmentState attachState
        {
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
        };
        VkPipelineColorBlendStateCreateInfo blendInfo
        {
                .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .attachmentCount = 1,
                .pAttachments = &attachState
        };

        //begin dynamic rendering
        vector<VkDynamicState> dynamicState
        {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicStateInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<u32>(dynamicState.size()),
            .pDynamicStates = dynamicState.data()
        };

        //structure required for dynamic rendering
        VkPipelineRenderingCreateInfo renderInfo
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &SWAPCHAIN_FORMAT,
            .depthAttachmentFormat = DEPTH_FORMAT
        };

        //create the graphics pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo
        {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderInfo,
            .stageCount = static_cast<u32>(shaderStages.size()),
            .pStages = shaderStages.data(),
            .pVertexInputState = &vertInputInfo,
            .pInputAssemblyState = &inputAssemblyInfo,
            .pViewportState = &viewportInfo,
            .pRasterizationState = &rasterizationInfo,
            .pMultisampleState = &multisampleInfo,
            .pDepthStencilState = &stencilInfo,
            .pColorBlendState = &blendInfo,
            .pDynamicState = &dynamicStateInfo,
            .layout = pipelinelayout,
            .renderPass = VK_NULL_HANDLE
        };

        auto pipeline_result = VulkanPipeline::create_graphics_dynamic(this->logicalDevice.vk_handle(), VK_NULL_HANDLE, pipelineInfo);
        if (!pipeline_result.has_value()) [[unlikely]] {
            return renderer_error(pipeline_result.error().code,
                                  format("Failed to create Vulkan graphics pipeline: {}", pipeline_result.error().message));
        }

        this->graphicsPipeline = std::move(*pipeline_result);

        Foundation::log_info("Vulkan graphics pipeline created.");
        return {};
    }

    RendererResult VulkanBackend::createSyncResources(const RendererCreateInfo &init) {
        const u32 frame_count = sanitize_frames_in_flight(init.features.desired_frames_in_flight);
        const u64 initial_timeline_value = frame_count;

        auto timeline_result = VulkanSemaphore::create_timeline(this->logicalDevice.vk_handle(), initial_timeline_value);
        if (!timeline_result.has_value()) [[unlikely]] {
            return renderer_error(timeline_result.error().code,
                                  format("Failed to create frame timeline semaphore: {}", timeline_result.error().message));
        }

        vector<FrameResources> frameResources;
        frameResources.reserve(frame_count);
        for (u32 frame_index = 0; frame_index < frame_count; ++frame_index) {
            FrameResources resources{};

            auto image_acquired_result = VulkanSemaphore::create_binary(this->logicalDevice.vk_handle());
            if (!image_acquired_result.has_value()) [[unlikely]] {
                return renderer_error(image_acquired_result.error().code,
                                      format("Failed to create image-acquired semaphore for frame {}: {}",
                                             frame_index,
                                             image_acquired_result.error().message));
            }

            resources.imageAcquiredSemaphore = std::move(*image_acquired_result);
            frameResources.push_back(std::move(resources));
        }

        this->frameTimelineSemaphore = std::move(*timeline_result);
        this->frameResources_ = std::move(frameResources);

        Foundation::log_info("Vulkan frame sync resources created: frames={} timeline_initial={}",
                             frame_count,
                             initial_timeline_value);
        return {};
    }

    RendererResult VulkanBackend::createCommandBuffers(const RendererCreateInfo &init) {
        // TODO(renderer): command pool(s) on the graphics queue family plus one primary command
        // buffer per frame in flight.
        (void)init;
        return {};
    }

    RendererResult VulkanBackend::createVulkanInstance(const RendererCreateInfo &init) {
        if (auto res = volkInitialize(); res != VK_SUCCESS) {
            return renderer_error(RendererErrorCode::OperationFailed, "Volk failed to initialize");
        }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
        VkApplicationInfo appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = "SturdyEngine Application",
            .apiVersion = VULKAN_API_VERSION,
        };

        auto extension_res = init.window->required_vulkan_instance_extensions();
        if (!extension_res) [[unlikely]] {
            return renderer_error(RendererErrorCode::OperationFailed,
                                  "Failed to get Window extensions list for Vulkan");
        }
        vector<const char *> extensions = extension_res.value();
        vector<const char *> requestedLayers{};

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
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<u32>(requestedLayers.size()),
            .ppEnabledLayerNames = requestedLayers.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
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
        info.window = &window;
        info.descriptor.provider = to_surface_provider(window.backend_kind());
        info.descriptor.system = to_surface_system(native->system);
        info.descriptor.display = native->display;
        info.descriptor.window = native->window;
        info.descriptor.provider_window = *provider_window;
        info.framebuffer_extent = {framebuffer->x, framebuffer->y};
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

        const WindowId window_id = init.window->id();
        if (surfaces_.contains(window_id)) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                            "A Vulkan surface already exists for this window."});
        }

        // Create the platform-specific VkSurfaceKHR.
        VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
        switch (init.descriptor.provider) {
            case SurfaceProvider::SDL3:
                {
                    auto *sdl_window = static_cast<SDL_Window *>(init.descriptor.provider_window);
                    if (!SDL_Vulkan_CreateSurface(sdl_window, vulkan_instance, nullptr, &vk_surface)) {
                        return unexpected(RendererError{RendererErrorCode::InitializationFailed,
                                                        format("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError())});
                    }
                    break;
                }
            case SurfaceProvider::GLFW:
                {
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

        VulkanSurface vulkan_surface(vk_surface, init.descriptor, init.window, init.framebuffer_extent, sanitize_frames_in_flight(init.desired_frames_in_flight));
        try {
            surfaces_.emplace(window_id, std::move(vulkan_surface));
        } catch (const bad_alloc &) {
            vkDestroySurfaceKHR(vulkan_instance, vk_surface, nullptr);
            return unexpected(RendererError{RendererErrorCode::OutOfMemory,
                                            "Out of memory allocating a Vulkan render surface slot."});
        }

        Foundation::log_info("Vulkan surface created: provider={} system={} extent={}x{}",
                             surface_provider_name(init.descriptor.provider),
                             surface_system_name(init.descriptor.system),
                             init.framebuffer_extent.width,
                             init.framebuffer_extent.height);
        return RenderSurfaceHandle{window_id};
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initVulkan(const RendererCreateInfo &init) {
        if (auto result = this->createVulkanInstance(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create Vulkan instance: {}", result.error().message));
        }

        auto surface_info = surface_create_info_from_window(*init.window, init.features.desired_frames_in_flight);
        if (!surface_info) [[unlikely]] {
            return unexpected(surface_info.error());
        }

        auto surface = createSurface(*surface_info);
        if (!surface) [[unlikely]] {
            return unexpected(surface.error());
        }

        VulkanSurface *primary = surface_slot(*surface);
        VkSurfaceKHR primary_vk_surface = primary->vk_handle();

        if (auto result = this->findPhysicalDevice(init, primary_vk_surface); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to find physical GPU: {}", result.error().message));
        }

        if (auto result = this->discoverGraphicsQueue(init, primary_vk_surface); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to discover a valid graphics queue: {}", result.error().message));
        }

        if (auto result = this->createDevice(init, primary_vk_surface); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create logical device: {}", result.error().message));
        }

        if (auto result = this->initializeVMA(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to initialize VMA allocator: {}", result.error().message));
        }

        // Re-resolve the surface pointer: the map cannot rehash from any of the calls above
        // (none of them touch surfaces_), but doing this right before use keeps the pointer
        // provably valid regardless of future changes to those steps.
        primary = surface_slot(*surface);
        if (auto result = this->createSwapchain(init, *primary); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create swapchain: {}", result.error().message));
        }

        if (auto result = this->createShaders(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create shaders: {}", result.error().message));
        }

        if (auto result = this->createGraphicsPipeline(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create graphics pipeline: {}", result.error().message));
        }

        if (auto result = this->createSyncResources(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create sync resources: {}", result.error().message));
        }

        if (auto result = this->createCommandBuffers(init); !result.has_value()) [[unlikely]] {
            return renderer_error(result.error().code,
                                  format("Failed to create command buffers: {}", result.error().message));
        }

        return surface;
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initialize(const RendererCreateInfo &init) {
        create_info_ = init;

        if (!init.window) [[unlikely]] {
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

    RendererExpected<RenderSurfaceHandle> VulkanBackend::create_window_surface(Window &window, u32 desired_frames_in_flight) {
        if (!initialized_) [[unlikely]] {
            return unexpected(RendererError{RendererErrorCode::InitializationFailed,
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

        VulkanSurface *added = surface_slot(*surface);
        if (auto result = this->createSwapchain(create_info_, *added); !result.has_value()) [[unlikely]] {
            destroy_window_surface(*surface);
            return unexpected(RendererError{result.error().code,
                                            format("Failed to create swapchain for added window: {}", result.error().message)});
        }

        return surface;
    }

} // namespace SFT::Core::Vulkan

namespace SFT::Core {

    unique_ptr<EngineBackend> create_vulkan_backend() {
        return EngineBackend::create<Vulkan::VulkanBackend>();
    }

} // namespace SFT::Core
