// VulkanBackend device bring-up: physical device selection and scoring, graphics queue
// discovery, logical device creation with feature verification, and VMA initialization.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <format>
#include <string>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :VulkanAllocator;
import :VulkanBackend;
import :VulkanConstants;
import :VulkanDevice;
import :VulkanPhysicalDevice;
import :VulkanQueue;
import :GraphicsBackendError;
import :Renderer;
import Sturdy.Foundation;
import Sturdy.RHI;

using std::format;
using std::string;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace {

        [[nodiscard]] string feature_set_message(const RHI::FeatureSet &features) {
            string out;
            features.for_each([&](RHI::Feature feature) {
                if (!out.empty()) {
                    out += ", ";
                }
                out += RHI::feature_name(feature);
            });
            return out.empty() ? string{"none"} : out;
        }

    } // namespace

    RendererResult VulkanBackend::findPhysicalDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        (void)init;
        auto devices_result = VulkanPhysicalDevice::enumerate(vulkan_instance);
        if (!devices_result.has_value()) [[unlikely]] {
            return graphics_backend_error(devices_result.error().code, devices_result.error().message);
        }

        for (const auto &candidate : *devices_result) {
            // The engine logger is spdlog/{fmt}; UString/ustr now have fmt::formatter specializations, so
            // they log directly with no .view()/.cpp_string_view() adaptation at the call site.
            Foundation::log_info("Found GPU: {} [{}] ({}) ID={} score={:.1f}",
                                 candidate.name(),
                                 candidate.vendor_name(),
                                 candidate.type_name(),
                                 candidate.device_id(),
                                 candidate.score());
        }

        // enumerate() guarantees a non-empty list, so max_element always dereferences a valid device.
        auto best = std::ranges::max_element(*devices_result, {}, &VulkanPhysicalDevice::score);
        physicalDevice = std::move(*best);
        Foundation::log_info("Selected GPU: {} [{}] driver={} Vulkan API={}",
                             physicalDevice.name(),
                             physicalDevice.vendor_name(),
                             physicalDevice.driver_version_string(),
                             physicalDevice.api_version_string());

        // make sure we support the swapchain format we plan to use
        auto surface_formats_result = this->physicalDevice.surface_formats(primary_surface);
        if (!surface_formats_result.has_value()) [[unlikely]] {
            return graphics_backend_error(surface_formats_result.error().code,
                                  format("Physical Device Selection failed at checking surface formats: {}",
                                         surface_formats_result.error().message));
        }

        if (!std::ranges::contains(*surface_formats_result, SWAPCHAIN_FORMAT, &VkSurfaceFormatKHR::format)) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "Physical Device Selection failed at checking surface formats");
        }

        return {};
    }

    RendererResult VulkanBackend::discoverGraphicsQueue(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        (void)init;
        if (auto res = this->physicalDevice.findGraphicsQueue(primary_surface); !res.has_value()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "Your GPU is apparently not Vulkan Compliant!! the Vulkan spec guarantees one graphics queue and we found zero");
        }
        Foundation::log_info("Successfully got a graphics queue from the physical device!");
        return {};
    }

    RendererResult VulkanBackend::createDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        (void)init;

        // Query which features the physical device actually supports.
        VkPhysicalDeviceMeshShaderFeaturesEXT supportedMeshFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, .pNext = nullptr};
        VkPhysicalDeviceVulkan14Features supportedFeatures14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = &supportedMeshFeatures};
        VkPhysicalDeviceVulkan13Features supportedFeatures13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &supportedFeatures14};
        VkPhysicalDeviceVulkan12Features supportedFeatures12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &supportedFeatures13};
        VkPhysicalDeviceVulkan11Features supportedFeatures11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &supportedFeatures12};
        VkPhysicalDeviceFeatures2 supportedFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &supportedFeatures11};
        this->physicalDevice.query_features2(supportedFeatures);

        if (not supportedFeatures13.dynamicRendering or not supportedFeatures13.synchronization2 or
            not supportedFeatures12.timelineSemaphore or not supportedFeatures12.bufferDeviceAddress) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                  "Required Vulkan features missing: dynamicRendering, synchronization2, timelineSemaphore, and bufferDeviceAddress are all required.");
        }

        // Slang emits SPV_KHR_shader_draw_parameters (gl_BaseVertex/gl_BaseInstance) for entry
        // points reading SV_VertexID/SV_InstanceID, so this is required for our triangle shader
        // even though it never reads a base value — without it validation rejects the module.
        if (not supportedFeatures11.shaderDrawParameters) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                  "Required Vulkan feature missing: shaderDrawParameters.");
        }

        RHI::FeatureSet supported_rhi_features = RHI::features_of({
            RHI::Feature::TimelineSynchronization,
            RHI::Feature::Synchronization2,
            RHI::Feature::DynamicRendering,
            RHI::Feature::ShaderDrawParameters,
            RHI::Feature::BufferDeviceAddress,
        });
        const bool supports_mesh_shader = this->physicalDevice.supports_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME) &&
                                          supportedMeshFeatures.meshShader;
        const bool supports_task_shader = supports_mesh_shader && supportedMeshFeatures.taskShader;
        if (supports_mesh_shader) {
            supported_rhi_features.set(RHI::Feature::MeshShader);
        }
        if (supports_task_shader) {
            supported_rhi_features.set(RHI::Feature::TaskShader);
        }
        if (this->physicalDevice.supports_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
            supported_rhi_features.set(RHI::Feature::AccelerationStructures);
        }
        if (this->physicalDevice.supports_extension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) {
            supported_rhi_features.set(RHI::Feature::RayTracingPipeline);
        }
        if (this->physicalDevice.supports_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME)) {
            supported_rhi_features.set(RHI::Feature::RayQuery);
        }
        if (init.features.prefer_async_compute && this->physicalDevice.find_compute_queue_family().has_value()) {
            supported_rhi_features.set(RHI::Feature::AsyncCompute);
        }

        RHI::FeatureSet required_rhi_features = init.features.required_rhi_features |
            RHI::features_of({
                RHI::Feature::TimelineSynchronization,
                RHI::Feature::Synchronization2,
                RHI::Feature::DynamicRendering,
                RHI::Feature::ShaderDrawParameters,
            });
        RHI::FeatureSet optional_rhi_features = init.features.optional_rhi_features;
        if (init.features.raytracing) {
            optional_rhi_features.set(RHI::Feature::RayTracingPipeline)
                .set(RHI::Feature::RayQuery)
                .set(RHI::Feature::AccelerationStructures)
                .set(RHI::Feature::BufferDeviceAddress);
        }
        if (init.features.prefer_async_compute) {
            optional_rhi_features.set(RHI::Feature::AsyncCompute);
        }

        feature_report_ = RHI::negotiate_features(supported_rhi_features, required_rhi_features, optional_rhi_features);
        if (!feature_report_.required_satisfied()) {
            return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                          format("Required RHI features are unavailable: {}",
                                                 feature_set_message(feature_report_.missing_required_features)));
        }
        Foundation::log_info("RHI feature negotiation: required enabled=[{}], optional enabled=[{}], optional unavailable=[{}]",
                             feature_set_message(feature_report_.enabled_required_features),
                             feature_set_message(feature_report_.enabled_optional_features),
                             feature_set_message(feature_report_.unavailable_optional_features));

        const RHI::FeatureSet enabled_rhi_features = feature_report_.enabled_features();
        capabilities_.timeline_semaphores = enabled_rhi_features.has(RHI::Feature::TimelineSynchronization);
        capabilities_.async_compute = enabled_rhi_features.has(RHI::Feature::AsyncCompute);
        capabilities_.raytracing = enabled_rhi_features.has(RHI::Feature::RayTracingPipeline) || enabled_rhi_features.has(RHI::Feature::RayQuery);
        capabilities_.mesh_shaders = enabled_rhi_features.has(RHI::Feature::MeshShader);
        capabilities_.max_frames_in_flight = sanitize_frames_in_flight(init.features.desired_frames_in_flight);

        const bool enable_mesh_shader = enabled_rhi_features.has(RHI::Feature::MeshShader);
        const bool enable_task_shader = enabled_rhi_features.has(RHI::Feature::TaskShader);

        // Build the enable chain — only request what we verified above.
        VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            .pNext = nullptr,
            .taskShader = enable_task_shader ? VK_TRUE : VK_FALSE,
            .meshShader = enable_mesh_shader ? VK_TRUE : VK_FALSE,
        };
        VkPhysicalDeviceVulkan14Features features14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, .pNext = enable_mesh_shader ? &meshFeatures : nullptr};
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
            .bufferDeviceAddress = VK_TRUE,
        };
        VkPhysicalDeviceVulkan11Features features11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext = &features12,
            .shaderDrawParameters = VK_TRUE,
        };
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features11};

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
        if (enable_mesh_shader) {
            extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        }

        // The Vulkan spec requires enabling VK_KHR_portability_subset on any device that
        // advertises it — MoltenVK and other non-conformant implementations use it to report
        // which core features they can't fully provide. Omitting it despite support is invalid.
        if (this->physicalDevice.supports_extension(PORTABILITY_SUBSET_EXTENSION_NAME)) {
            extensions.push_back(PORTABILITY_SUBSET_EXTENSION_NAME);
        }

        VulkanDevice::DeviceCreateDesc desc{
            .graphics_queue_family = gfx_family,
            .present_queue_family = present_family,
            .extensions = extensions,
            .features_pnext = &features,
        };

        auto device_result = VulkanDevice::create(this->physicalDevice.vk_handle(), desc);
        if (!device_result.has_value()) [[unlikely]] {
            return graphics_backend_error(device_result.error().code,
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
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "Failed to get a graphics queue for drawing graphics");
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
            .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        };

        auto allocator_result = VulkanAllocator::create(desc);
        if (!allocator_result.has_value()) [[unlikely]] {
            return graphics_backend_error(allocator_result.error().code,
                                  format("Failed to start VMA: {}", allocator_result.error().message));
        }

        this->vmaAllocator = std::move(*allocator_result);

        Foundation::log_info("VMA Initialization was a success!");
        return {};
    }

} // namespace SFT::Core::Vulkan
