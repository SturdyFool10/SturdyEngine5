// VulkanBackend lifecycle: construction, capability/GPU queries, the initialize()/initVulkan()
// bring-up sequence, and ordered teardown. Sibling VulkanBackend*.cpp files implement the
// subsystem steps this file orchestrates.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"

#include <expected>
#include <format>
#include <memory>
#include <optional>
#pragma endregion

module Sturdy.Core;

import :VulkanAllocator;
import :VulkanBackend;
import :VulkanDevice;
import :VulkanPhysicalDevice;
import :VulkanQueue;
import :VulkanSurface;
import :GraphicsBackendError;
import :Renderer;
import :RenderSurface;
import Sturdy.Foundation;
import Sturdy.Platform;

using std::format;
using std::nullopt;
using std::optional;
using std::unexpected;
using std::unique_ptr;

namespace SFT::Core::Vulkan {

    VulkanBackend::VulkanBackend(ConstructorKey key)
        : EngineBackend(key) {}

    RendererCapabilities VulkanBackend::capabilities() const noexcept {
        return capabilities_;
    }

    optional<GpuInfo> VulkanBackend::gpu_info() const {
        // Translate the cached VkPhysicalDeviceProperties into the API-agnostic GpuInfo. Everything
        // here is already decoded to strings/ints by VulkanPhysicalDevice, so no Vulkan types leak.
        if (!physicalDevice.is_valid()) {
            return nullopt; // No device selected yet (before a successful initialize()).
        }
        GpuInfo info{};
        // GpuInfo stays std::string (the API-agnostic boundary type), so each UString/ustr is lowered with
        // .cpp_string(). The cpp_ prefix flags the crossing back into standard types at the call site.
        info.name = physicalDevice.name().cpp_string();
        info.vendor = physicalDevice.vendor_name();
        info.driver_version = physicalDevice.driver_version_string().cpp_string();
        info.api_version = physicalDevice.api_version_string().cpp_string();
        info.device_type = physicalDevice.type_name();
        info.vendor_id = physicalDevice.vendor_id();
        info.device_id = physicalDevice.device_id();
        return info;
    }

    void VulkanBackend::wait_idle() noexcept {
        if (logicalDevice.is_valid()) {
            logicalDevice.wait_idle();
        }
    }

    void VulkanBackend::destroyVulkanResources() noexcept {
        wait_idle();
        rhiDevice.reset();
        destroy_all_surfaces();
        vmaAllocator.destroy();
        logicalDevice.destroy();
        gfxQueue = VulkanQueue{};
        physicalDevice = VulkanPhysicalDevice{};
        if (vulkan_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(vulkan_instance, nullptr);
            vulkan_instance = VK_NULL_HANDLE;
        }
        if (volk_initialized_) {
            volkFinalize();
            volk_initialized_ = false;
        }
    }

    VulkanBackend::~VulkanBackend() {
        // RAII teardown: drain all in-flight work first, then release GPU objects in
        // reverse creation order (surfaces → shaders/pipelines/layouts → allocator → device → instance). This must happen
        // explicitly and in this order, since automatic member destruction would otherwise run
        // *after* this body (i.e. after vkDestroyInstance below) and tear the allocator/device
        // down against an already-destroyed instance.
        destroyVulkanResources();
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initVulkan(const RendererCreateInfo &init) {
        if (auto result = this->createVulkanInstance(init); !result.has_value()) [[unlikely]] {
            return graphics_backend_error(result.error().code,
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
            return graphics_backend_error(result.error().code,
                                  format("Failed to find physical GPU: {}", result.error().message));
        }

        if (auto result = this->discoverGraphicsQueue(init, primary_vk_surface); !result.has_value()) [[unlikely]] {
            return graphics_backend_error(result.error().code,
                                  format("Failed to discover a valid graphics queue: {}", result.error().message));
        }

        if (auto result = this->createDevice(init, primary_vk_surface); !result.has_value()) [[unlikely]] {
            return graphics_backend_error(result.error().code,
                                  format("Failed to create logical device: {}", result.error().message));
        }

        if (auto result = this->initializeVMA(init); !result.has_value()) [[unlikely]] {
            return graphics_backend_error(result.error().code,
                                  format("Failed to initialize VMA allocator: {}", result.error().message));
        }

        installRhiBridge();

        return surface;
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initialize(const RendererCreateInfo &init) {
        create_info_ = init;

        if (!init.window) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                            "Vulkan backend requires a window to create its primary surface."});
        }

        // Mark initialized before initVulkan so it can create the backend-owned primary surface
        // during bring-up (the surface is needed to query present support). Reset on failure.
        initialized_ = true;

        auto primary_surface = this->initVulkan(init);
        if (!primary_surface.has_value()) [[unlikely]] {
            const auto error = primary_surface.error();
            destroyVulkanResources();
            initialized_ = false;
            return graphics_backend_error(error.code,
                                  format("Initializing Vulkan has failed: {}", error.message));
        }

        return *primary_surface;
    }

} // namespace SFT::Core::Vulkan

namespace SFT::Core {

    unique_ptr<EngineBackend> create_vulkan_backend() {
        return EngineBackend::create<Vulkan::VulkanBackend>();
    }

} // namespace SFT::Core
