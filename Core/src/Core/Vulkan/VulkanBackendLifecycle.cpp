


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

#include <Foundation/src/Foundation.hpp>
#include <RHI/Threading.hpp>

#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/VulkanSurface.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <Core/RenderSurface.hpp>
#include <Platform/Platform.hpp>

#include <tracy/Tracy.hpp>

using std::format;
using std::nullopt;
using std::optional;
using std::unexpected;
using std::unique_ptr;

namespace SFT::Core::Vulkan {

    VulkanBackend::VulkanBackend(ConstructorKey key)
        : EngineBackend(key) {}

    RendererCapabilities VulkanBackend::capabilities() const noexcept {
        ZoneScopedN("VulkanBackend::VulkanBackend");
        return capabilities_;
    }

    RHI::RenderThreadingCapabilities VulkanBackend::render_threading_capabilities() const noexcept {
        ZoneScopedN("VulkanBackend::render_threading_capabilities");
        return RHI::RenderThreadingCapabilities{
            .backend_allows_dedicated_render_thread = true,






            .backend_allows_parallel_command_recording = true,
            .platform_allows_threads = RHI::compile_time_rhi_multithreading_allowed,
            .requires_graphics_calls_on_owner_thread = true,
            .recommended_mode = RHI::choose_render_threading_mode(RHI::RenderThreadingCapabilities{
                .backend_allows_dedicated_render_thread = true,
                .backend_allows_parallel_command_recording = true,
                .platform_allows_threads = RHI::compile_time_rhi_multithreading_allowed,
                .requires_graphics_calls_on_owner_thread = true,
            }),
        };
    }

    optional<GpuInfo> VulkanBackend::gpu_info() const {
        ZoneScopedN("VulkanBackend::gpu_info");


        if (!physicalDevice.is_valid()) {
            return nullopt;
        }
        GpuInfo info{};


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
        ZoneScopedN("VulkanBackend::wait_idle");
        if (logicalDevice.is_valid()) {
            logicalDevice.wait_idle();
        }
    }

    void VulkanBackend::destroyVulkanResources() noexcept {
        ZoneScopedN("VulkanBackend::destroyVulkanResources");
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

    }

    VulkanBackend::~VulkanBackend() {
        ZoneScopedN("VulkanBackend::~VulkanBackend");





        destroyVulkanResources();
    }

    RendererExpected<RenderSurfaceHandle> VulkanBackend::initVulkan(const RendererCreateInfo &init) {
        ZoneScopedN("VulkanBackend::initVulkan");
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
        ZoneScopedN("VulkanBackend::initialize");
        if (initialized_) {
            return graphics_backend_error(
                GraphicsBackendErrorCode::OperationFailed,
                "Vulkan backend is already initialized.");
        }
        create_info_ = init;

        if (!init.window) [[unlikely]] {
            return unexpected(GraphicsBackendError{GraphicsBackendErrorCode::InitializationFailed,
                                            "Vulkan backend requires a window to create its primary surface."});
        }



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
        return EngineBackend::create_erased<Vulkan::VulkanBackend>();
    }

} // namespace SFT::Core
