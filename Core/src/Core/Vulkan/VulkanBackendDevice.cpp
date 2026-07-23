// VulkanBackend device bring-up: physical device selection and scoring, graphics queue
// discovery, logical device creation with feature verification, and VMA initialization.
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
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanConstants.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <RHI/RHI.hpp>

using std::format;
using std::optional;
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

        [[nodiscard]] u32 available_queue_count(const VulkanPhysicalDevice &device, optional<u32> family) noexcept {
            if (!family || *family >= device.queue_families().size()) {
                return 0;
            }
            return device.queue_families()[*family].queueCount;
        }

        [[nodiscard]] u32 preferred_lane_count(const VulkanPhysicalDevice &device, optional<u32> family) noexcept {
            const u32 count = available_queue_count(device, family);
            return count == 0 ? 0 : std::min(2u, count);
        }

        [[nodiscard]] optional<u32> find_dedicated_queue_family(const VulkanPhysicalDevice &device,
                                                               VkQueueFlags required,
                                                               VkQueueFlags forbidden) noexcept {
            const auto &families = device.queue_families();
            for (u32 i = 0; i < static_cast<u32>(families.size()); ++i) {
                const VkQueueFlags flags = families[i].queueFlags;
                if ((flags & required) == required && (flags & forbidden) == 0 && families[i].queueCount > 0) {
                    return i;
                }
            }
            return {};
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
        if (supportedFeatures.features.imageCubeArray) {
            supported_rhi_features.set(RHI::Feature::ImageCubeArray);
        }
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
        const auto probed_gfx_family = this->physicalDevice.findGraphicsQueue(primary_surface);
        const auto probed_dedicated_compute_family = find_dedicated_queue_family(
            this->physicalDevice, VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT);
        auto probed_dedicated_transfer_family = find_dedicated_queue_family(
            this->physicalDevice, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        if (!probed_dedicated_transfer_family.has_value()) {
            // Some GPUs expose a distinct async compute family that also supports transfer, but no pure
            // DMA/copy family. It is still useful for RHI Transfer work because it is distinct from graphics.
            probed_dedicated_transfer_family = find_dedicated_queue_family(
                this->physicalDevice, VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT);
        }
        const auto probed_sparse_family = this->physicalDevice.find_sparse_binding_queue_family();
        const auto probed_video_decode_family = this->physicalDevice.find_video_decode_queue_family();
        const auto probed_video_encode_family = this->physicalDevice.find_video_encode_queue_family();
        const bool supports_async_compute_queue = probed_gfx_family.has_value() && probed_dedicated_compute_family.has_value() &&
                                                  *probed_dedicated_compute_family != *probed_gfx_family;
        const bool supports_async_transfer_queue = probed_gfx_family.has_value() && probed_dedicated_transfer_family.has_value() &&
                                                   *probed_dedicated_transfer_family != *probed_gfx_family;
        const bool supports_sparse_queue = supportedFeatures.features.sparseBinding && probed_sparse_family.has_value();
        const bool supports_video_decode_queue = this->physicalDevice.supports_extension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) &&
                                                 this->physicalDevice.supports_extension(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) &&
                                                 probed_video_decode_family.has_value();
        const bool supports_video_encode_queue = this->physicalDevice.supports_extension(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) &&
                                                 this->physicalDevice.supports_extension(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME) &&
                                                 probed_video_encode_family.has_value();
        if (supports_async_compute_queue) {
            supported_rhi_features.set(RHI::Feature::AsyncCompute);
        }
        if (supports_async_transfer_queue) {
            supported_rhi_features.set(RHI::Feature::AsyncTransfer);
        }
        if (supports_sparse_queue) {
            supported_rhi_features.set(RHI::Feature::SparseBinding);
        }
        if (supports_video_decode_queue) {
            supported_rhi_features.set(RHI::Feature::VideoDecodeQueue);
        }
        if (supports_video_encode_queue) {
            supported_rhi_features.set(RHI::Feature::VideoEncodeQueue);
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
        if (supports_async_compute_queue) {
            optional_rhi_features.set(RHI::Feature::AsyncCompute);
        }
        if (supports_async_transfer_queue) {
            optional_rhi_features.set(RHI::Feature::AsyncTransfer);
        }
        if (supports_sparse_queue) {
            optional_rhi_features.set(RHI::Feature::SparseBinding);
        }
        if (supports_video_decode_queue) {
            optional_rhi_features.set(RHI::Feature::VideoDecodeQueue);
        }
        if (supports_video_encode_queue) {
            optional_rhi_features.set(RHI::Feature::VideoEncodeQueue);
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
        // Anisotropic filtering: enable whenever the device supports it, so the RHI sampler path can
        // honor SamplerDesc::max_anisotropy (clamped to the device limit at create_sampler time).
        // Near-universal on real GPUs; guarded because portability-subset implementations may lack it.
        if (supportedFeatures.features.samplerAnisotropy) {
            features.features.samplerAnisotropy = VK_TRUE;
        }
        if (enabled_rhi_features.has(RHI::Feature::ImageCubeArray)) {
            features.features.imageCubeArray = VK_TRUE;
        }

        // Discover queue families. Graphics was already verified by discoverGraphicsQueue;
        // present may share the same index — VulkanDevice::create() deduplicates automatically.
        auto gfx_family = this->physicalDevice.findGraphicsQueue(primary_surface);
        auto present_family = this->physicalDevice.find_present_queue_family(primary_surface);
        optional<u32> compute_family = enabled_rhi_features.has(RHI::Feature::AsyncCompute)
            ? probed_dedicated_compute_family
            : optional<u32>{};
        optional<u32> transfer_family = enabled_rhi_features.has(RHI::Feature::AsyncTransfer)
            ? probed_dedicated_transfer_family
            : optional<u32>{};
        optional<u32> sparse_family = enabled_rhi_features.has(RHI::Feature::SparseBinding)
            ? probed_sparse_family
            : optional<u32>{};
        optional<u32> video_decode_family = enabled_rhi_features.has(RHI::Feature::VideoDecodeQueue)
            ? probed_video_decode_family
            : optional<u32>{};
        optional<u32> video_encode_family = enabled_rhi_features.has(RHI::Feature::VideoEncodeQueue)
            ? probed_video_encode_family
            : optional<u32>{};
        if (compute_family.has_value() && transfer_family.has_value() && *compute_family == *transfer_family) {
            // VulkanDevice wraps queue handles with per-wrapper mutexes. If transfer aliases the compute
            // queue family, request/retrieve it once and let the RHI bridge map Transfer to computeQueue.
            transfer_family.reset();
        }

        // Extensions: swapchain (required for presentation) + calibrated timestamps
        // (Vulkan 1.4 core, needed for anchoring GPU timer to wall clock).
        hdr_metadata_enabled_ = false;
        vector<const char *> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
        };
        if (enable_mesh_shader) {
            extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        }
        if (video_decode_family.has_value() || video_encode_family.has_value()) {
            extensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        }
        if (video_decode_family.has_value()) {
            extensions.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        }
        if (video_encode_family.has_value()) {
            extensions.push_back(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
        }
        if (static_cast<bool>(init.features.presentation.hdr_enabled) &&
            this->physicalDevice.supports_extension(VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
            hdr_metadata_enabled_ = true;
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
            .compute_queue_family = compute_family,
            .transfer_queue_family = transfer_family,
            .sparse_queue_family = sparse_family,
            .video_decode_queue_family = video_decode_family,
            .video_encode_queue_family = video_encode_family,
            .graphics_queue_count = preferred_lane_count(this->physicalDevice, gfx_family),
            .compute_queue_count = preferred_lane_count(this->physicalDevice, compute_family),
            .transfer_queue_count = preferred_lane_count(this->physicalDevice, transfer_family),
            .sparse_queue_count = preferred_lane_count(this->physicalDevice, sparse_family),
            .video_decode_queue_count = preferred_lane_count(this->physicalDevice, video_decode_family),
            .video_encode_queue_count = preferred_lane_count(this->physicalDevice, video_encode_family),
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

        if (compute_family.has_value()) {
            if (!this->logicalDevice.compute_queue().has_value()) [[unlikely]] {
                return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "Failed to get a dedicated compute queue.");
            }
            Foundation::log_info("Dedicated Vulkan compute queue selected: family={}", *compute_family);
        }

        if (transfer_family.has_value()) {
            if (!this->logicalDevice.transfer_queue().has_value()) [[unlikely]] {
                return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "Failed to get a dedicated transfer queue.");
            }
            Foundation::log_info("Dedicated Vulkan transfer queue selected: family={} lanes={}", *transfer_family,
                                 this->logicalDevice.transfer_queue_lanes().size());
        }
        if (compute_family.has_value()) {
            Foundation::log_info("Vulkan compute queue lanes={}", this->logicalDevice.compute_queue_lanes().size());
        }
        Foundation::log_info("Vulkan graphics queue lanes={}", this->logicalDevice.graphics_queue_lanes().size());
        if (sparse_family.has_value()) {
            Foundation::log_info("Vulkan sparse queue selected: family={} lanes={}", *sparse_family,
                                 this->logicalDevice.sparse_queue_lanes().size());
        }
        if (video_decode_family.has_value()) {
            Foundation::log_info("Vulkan video decode queue selected: family={} lanes={}", *video_decode_family,
                                 this->logicalDevice.video_decode_queue_lanes().size());
        }
        if (video_encode_family.has_value()) {
            Foundation::log_info("Vulkan video encode queue selected: family={} lanes={}", *video_encode_family,
                                 this->logicalDevice.video_encode_queue_lanes().size());
        }
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
