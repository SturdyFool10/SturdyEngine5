

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
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <Foundation/Foundation.hpp>

#include <tracy/Tracy.hpp>

#include <Core/GraphicsBackendError.hpp>
#include <Core/Renderer.hpp>
#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanConstants.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <RHI/RHI.hpp>

using std::format;
using std::optional;
using std::string;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace {

        /// Performs the feature set message operation for `Vulkan` using the supplied arguments.
        ///
        /// @param features `features` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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

        /// Largest heap backing a memory type that is both DEVICE_LOCAL and HOST_VISIBLE, in bytes, or
        /// 0 if no such type exists.
        ///
        /// @param memory Physical device memory properties to scan.
        ///
        /// @return See summary above.
        /// @note A pre-Resizable-BAR system typically exposes no such type at all, or only a small
        ///       (~256 MiB) legacy aperture; Resizable BAR exposes a much larger one (often the GPU's
        ///       full VRAM). This is purely a diagnostic — VMA already opportunistically places
        ///       suitable DeviceLocal buffer allocations there on its own (see VulkanRhiConvert.hpp's
        ///       to_vma()); nothing here gates that behavior.
        /// @note This function does not throw exceptions.
        [[nodiscard]] VkDeviceSize rebar_heap_size_bytes(const VkPhysicalDeviceMemoryProperties &memory) noexcept {
            VkDeviceSize largest = 0;
            constexpr VkMemoryPropertyFlags required =
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            for (u32 type_index = 0; type_index < memory.memoryTypeCount; ++type_index) {
                const VkMemoryType &type = memory.memoryTypes[type_index];
                if ((type.propertyFlags & required) != required) {
                    continue;
                }
                largest = std::max(largest, memory.memoryHeaps[type.heapIndex].size);
            }
            return largest;
        }

        /// Returns the available queue count for this `Vulkan`.
        ///
        /// @param device Device used or affected by the operation.
        /// @param family `family` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 available_queue_count(const VulkanPhysicalDevice &device, optional<u32> family) noexcept {
            if (!family || *family >= device.queue_families().size()) {
                return 0;
            }
            return device.queue_families()[*family].queueCount;
        }

        /// Returns the preferred lane count for this `Vulkan`.
        ///
        /// @param device Device used or affected by the operation.
        /// @param family `family` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 preferred_lane_count(const VulkanPhysicalDevice &device, optional<u32> family) noexcept {
            const u32 count = available_queue_count(device, family);
            return count == 0 ? 0 : std::min(2u, count);
        }

        /// Performs the physical device ID operation for `Vulkan` using the supplied arguments.
        ///
        /// @param device Device used or affected by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::string physical_device_id(const VulkanPhysicalDevice &device) {
            VkPhysicalDeviceIDProperties ids{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &ids,
            };
            vkGetPhysicalDeviceProperties2(device.vk_handle(), &properties);
            if (ids.deviceLUIDValid != VK_TRUE) {
                return {};
            }
            u64 bits = 0;
            static_assert(VK_LUID_SIZE == sizeof(bits));
            std::memcpy(&bits, ids.deviceLUID, sizeof(bits));
            return format("windows-luid:{:016x}", bits);
        }

        /// Finds dedicated queue family in the available state.
        ///
        /// @param device Device used or affected by the operation.
        /// @param required `required` value used by the operation.
        /// @param forbidden `forbidden` value used by the operation.
        ///
        /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
        /// @note This function does not throw exceptions.
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

    /// Finds the requested entry in the available state.
    ///
    /// @param init `init` value used by the operation.
    /// @param primary_surface Surface used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::Unsupported`, `GraphicsBackendErrorCode::InitializationFailed`.
    RendererResult VulkanBackend::findPhysicalDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        ZoneScopedN("VulkanBackend::findPhysicalDevice");
        (void)init;
        auto devices_result = VulkanPhysicalDevice::enumerate(vulkan_instance);
        if (!devices_result.has_value()) [[unlikely]] {
            return graphics_backend_error(devices_result.error().code, devices_result.error().message);
        }

        for (const auto &candidate : *devices_result) {


            Foundation::log_info("Found GPU: {} [{}] ({}) ID={} score={:.1f}",
                                 candidate.name(),
                                 candidate.vendor_name(),
                                 candidate.type_name(),
                                 candidate.device_id(),
                                 candidate.score());
        }

        auto best = devices_result->end();
        if (!init.physical_device_id.empty()) {
            best = std::ranges::find_if(*devices_result, [&](const VulkanPhysicalDevice &candidate) {
                return physical_device_id(candidate) == init.physical_device_id;
            });
            if (best == devices_result->end()) {
                return graphics_backend_error(GraphicsBackendErrorCode::Unsupported,
                                              "The selected physical GPU is not available through Vulkan.");
            }
        } else {
            best = std::ranges::max_element(*devices_result, {}, &VulkanPhysicalDevice::score);
        }
        physicalDevice = std::move(*best);
        Foundation::log_info("Selected GPU: {} [{}] driver={} Vulkan API={}",
                             physicalDevice.name(),
                             physicalDevice.vendor_name(),
                             physicalDevice.driver_version_string(),
                             physicalDevice.api_version_string());


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

    /// Performs the discover graphics queue operation for `Vulkan` using the supplied arguments.
    ///
    /// @param init `init` value used by the operation.
    /// @param primary_surface Surface used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`.
    RendererResult VulkanBackend::discoverGraphicsQueue(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        ZoneScopedN("VulkanBackend::discoverGraphicsQueue");
        (void)init;
        if (auto res = this->physicalDevice.findGraphicsQueue(primary_surface); !res.has_value()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "Your GPU is apparently not Vulkan Compliant!! the Vulkan spec guarantees one graphics queue and we found zero");
        }
        Foundation::log_info("Successfully got a graphics queue from the physical device!");
        return {};
    }

    /// Performs the create device operation for `Vulkan` using the supplied arguments.
    ///
    /// @param init `init` value used by the operation.
    /// @param primary_surface Surface used or affected by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `GraphicsBackendErrorCode::InitializationFailed`, `GraphicsBackendErrorCode::Unsupported`.
    RendererResult VulkanBackend::createDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        ZoneScopedN("VulkanBackend::createDevice");
        (void)init;


        VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR supportedPresentModeFifoLatestReadyFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
            .pNext = nullptr};


        // Shader Execution Reordering. Note this only ever gates whether shader code may use the
        // reorder intrinsic (Slang/HLSL/GLSL HitObject-style API) -- there is no CPU-side command or
        // pipeline state for it, unlike every other feature this session. See the feature-detection
        // block below for the fuller caveat on this extension's confidence level.
        VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT supportedInvocationReorderFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT,
            .pNext = &supportedPresentModeFifoLatestReadyFeatures,
        };
        VkPhysicalDeviceOpacityMicromapFeaturesEXT supportedOpacityMicromapFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT,
            .pNext = &supportedInvocationReorderFeatures,
        };
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR supportedFragmentShadingRateFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
            .pNext = &supportedOpacityMicromapFeatures,
        };
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR supportedSwapchainMaintenance1Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
            .pNext = &supportedFragmentShadingRateFeatures,
        };
        VkPhysicalDeviceRayQueryFeaturesKHR supportedRayQueryFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
            .pNext = &supportedSwapchainMaintenance1Features,
        };
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR supportedRayTracingPipelineFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
            .pNext = &supportedRayQueryFeatures,
        };
        VkPhysicalDeviceAccelerationStructureFeaturesKHR supportedAccelerationStructureFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            .pNext = &supportedRayTracingPipelineFeatures,
        };
        VkPhysicalDeviceMeshShaderFeaturesEXT supportedMeshFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            .pNext = &supportedAccelerationStructureFeatures,
        };
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


        if (not supportedFeatures11.shaderDrawParameters) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed,
                                          "Required Vulkan feature missing: shaderDrawParameters.");
        }

        RHI::FeatureSet supported_rhi_features = RHI::features_of({
            RHI::Feature::TimelineSynchronization,
            RHI::Feature::Synchronization2,
            RHI::Feature::DynamicRendering,
            RHI::Feature::ShaderDrawParameters,

            RHI::Feature::RenderBundles,
            RHI::Feature::BufferDeviceAddress,
        });
        if (supportedFeatures.features.imageCubeArray) {
            supported_rhi_features.set(RHI::Feature::ImageCubeArray);
        }
        if (supportedFeatures.features.depthBounds) {
            supported_rhi_features.set(RHI::Feature::DepthBoundsTest);
        }
        if (supportedFragmentShadingRateFeatures.pipelineFragmentShadingRate) {
            supported_rhi_features.set(RHI::Feature::VariableRateShading);
            supported_rhi_features.set(RHI::Feature::PipelineFragmentShadingRate);
        }
        if (supportedFragmentShadingRateFeatures.primitiveFragmentShadingRate) {
            supported_rhi_features.set(RHI::Feature::PrimitiveFragmentShadingRate);
        }
        if (supportedFragmentShadingRateFeatures.attachmentFragmentShadingRate) {
            supported_rhi_features.set(RHI::Feature::AttachmentFragmentShadingRate);
        }
        const bool supports_bindless_descriptor_heap =
            supportedFeatures12.descriptorIndexing &&
            supportedFeatures12.runtimeDescriptorArray &&
            supportedFeatures12.descriptorBindingVariableDescriptorCount &&
            supportedFeatures12.descriptorBindingPartiallyBound &&
            supportedFeatures12.descriptorBindingSampledImageUpdateAfterBind &&
            supportedFeatures12.shaderSampledImageArrayNonUniformIndexing;
        if (supports_bindless_descriptor_heap) {
            supported_rhi_features
                .set(RHI::Feature::BindlessResources)
                .set(RHI::Feature::DescriptorIndexing)
                .set(RHI::Feature::RuntimeDescriptorArrays)
                .set(RHI::Feature::DescriptorBindingVariableCount)
                .set(RHI::Feature::DescriptorBindingPartiallyBound)
                .set(RHI::Feature::DescriptorBindingUpdateAfterBind)
                .set(RHI::Feature::NonUniformResourceIndexing)
                .set(RHI::Feature::SampledImageArrayNonUniformIndexing);
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
        const bool supports_acceleration_structures =
            this->physicalDevice.supports_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
            this->physicalDevice.supports_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) &&
            supportedAccelerationStructureFeatures.accelerationStructure;
        const bool supports_ray_tracing_pipeline = supports_acceleration_structures &&
                                                   this->physicalDevice.supports_extension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
                                                   supportedRayTracingPipelineFeatures.rayTracingPipeline;
        const bool supports_ray_query = supports_acceleration_structures &&
                                        this->physicalDevice.supports_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                                        supportedRayQueryFeatures.rayQuery;
        if (supports_acceleration_structures) {
            supported_rhi_features.set(RHI::Feature::AccelerationStructures);
        }
        if (supports_ray_tracing_pipeline) {
            supported_rhi_features.set(RHI::Feature::RayTracingPipeline);
        }
        if (supports_ray_query) {
            supported_rhi_features.set(RHI::Feature::RayQuery);
        }
        if (this->physicalDevice.supports_extension(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME) &&
            supportedPresentModeFifoLatestReadyFeatures.presentModeFifoLatestReady) {
            supported_rhi_features.set(RHI::Feature::PresentModeFifoLatestReady);
        }
        if (surface_maintenance1_enabled_ &&
            this->physicalDevice.supports_extension(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) &&
            supportedSwapchainMaintenance1Features.swapchainMaintenance1) {
            supported_rhi_features.set(RHI::Feature::SwapchainMaintenance);
        }
#if defined(_WIN32)


        if (this->physicalDevice.supports_extension("VK_KHR_external_memory_win32")) {
            supported_rhi_features.set(RHI::Feature::ExternalMemory).set(RHI::Feature::ExternalMemoryWin32);
        }
        if (this->physicalDevice.supports_extension("VK_KHR_external_semaphore_win32")) {
            supported_rhi_features.set(RHI::Feature::ExternalSemaphore).set(RHI::Feature::ExternalSemaphoreWin32);
        }


        if (surface_capabilities2_enabled_ &&
            this->physicalDevice.supports_extension("VK_EXT_full_screen_exclusive")) {
            supported_rhi_features.set(RHI::Feature::FullScreenExclusive);
        }
#endif
        if (this->physicalDevice.supports_extension("VK_EXT_conservative_rasterization")) {
            supported_rhi_features.set(RHI::Feature::ConservativeRasterization);
        }
        if (this->physicalDevice.supports_extension("VK_EXT_sample_locations")) {
            supported_rhi_features.set(RHI::Feature::SampleLocations);
        }
        // The vkGetPhysicalDeviceFeatures2 bit alone isn't sufficient here: unlike every other
        // extension in this file, these two weren't also gated on supports_extension(), so a driver
        // that answers the feature query with VK_TRUE for a struct type it doesn't truly implement
        // (observed on an AMD RX 9070, driver 2.0.395) got the extension requested at vkCreateDevice
        // time anyway, which Vulkan rejects outright with VK_ERROR_EXTENSION_NOT_PRESENT.
        if (supportedOpacityMicromapFeatures.micromap &&
            this->physicalDevice.supports_extension(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME)) {
            supported_rhi_features.set(RHI::Feature::OpacityMicromap);
        }
        // Unlike every other feature this session, enabling this bit does not unlock any RHI-level
        // command or pipeline state; it only tells shader code (Slang/HLSL/GLSL) that the
        // HitObject-style reorder intrinsic is safe to call. No Slang stdlib support for that
        // intrinsic exists in this engine yet, so setting this bit is necessary-but-not-sufficient
        // for a shader to actually use SER today -- see Feature::RayTracingInvocationReorder's own
        // doc comment in Features.hpp for the fuller picture.
        if (supportedInvocationReorderFeatures.rayTracingInvocationReorder &&
            this->physicalDevice.supports_extension(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME)) {
            supported_rhi_features.set(RHI::Feature::RayTracingInvocationReorder);
        }
        const auto probed_gfx_family = this->physicalDevice.findGraphicsQueue(primary_surface);
        const auto probed_dedicated_compute_family = find_dedicated_queue_family(
            this->physicalDevice,
            VK_QUEUE_COMPUTE_BIT,
            VK_QUEUE_GRAPHICS_BIT);
        auto probed_dedicated_transfer_family = find_dedicated_queue_family(
            this->physicalDevice,
            VK_QUEUE_TRANSFER_BIT,
            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        if (!probed_dedicated_transfer_family.has_value()) {


            probed_dedicated_transfer_family = find_dedicated_queue_family(
                this->physicalDevice,
                VK_QUEUE_TRANSFER_BIT,
                VK_QUEUE_GRAPHICS_BIT);
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
                .set(RHI::Feature::BufferDeviceAddress)
                .set(RHI::Feature::BindlessResources);

            // Both extensions declare VK_KHR_acceleration_structure as a hard dependency in the
            // Vulkan registry; requesting either one without acceleration structures also being
            // negotiated on produces an invalid device-extension combination that fails
            // vkCreateDevice outright (confirmed empirically: UiWorkbench, which doesn't request
            // raytracing, failed device creation while these were being requested unconditionally
            // below regardless of init.features.raytracing).
            optional_rhi_features.set(RHI::Feature::OpacityMicromap);
            optional_rhi_features.set(RHI::Feature::RayTracingInvocationReorder);
        }
        const auto close_ray_tracing_dependencies = [](RHI::FeatureSet &features) {
            if (features.has(RHI::Feature::RayQuery) || features.has(RHI::Feature::RayTracingPipeline)) {
                features.set(RHI::Feature::AccelerationStructures);
            }
            if (features.has(RHI::Feature::AccelerationStructures)) {
                features.set(RHI::Feature::BufferDeviceAddress);
            }
        };
        const auto close_bindless_dependencies = [](RHI::FeatureSet &features) {
            if (!features.has(RHI::Feature::BindlessResources) &&
                !features.has(RHI::Feature::DescriptorIndexing) &&
                !features.has(RHI::Feature::RuntimeDescriptorArrays) &&
                !features.has(RHI::Feature::DescriptorBindingVariableCount) &&
                !features.has(RHI::Feature::DescriptorBindingPartiallyBound) &&
                !features.has(RHI::Feature::DescriptorBindingUpdateAfterBind) &&
                !features.has(RHI::Feature::NonUniformResourceIndexing) &&
                !features.has(RHI::Feature::SampledImageArrayNonUniformIndexing)) {
                return;
            }
            features
                .set(RHI::Feature::BindlessResources)
                .set(RHI::Feature::DescriptorIndexing)
                .set(RHI::Feature::RuntimeDescriptorArrays)
                .set(RHI::Feature::DescriptorBindingVariableCount)
                .set(RHI::Feature::DescriptorBindingPartiallyBound)
                .set(RHI::Feature::DescriptorBindingUpdateAfterBind)
                .set(RHI::Feature::NonUniformResourceIndexing)
                .set(RHI::Feature::SampledImageArrayNonUniformIndexing);
        };
        close_ray_tracing_dependencies(required_rhi_features);
        close_ray_tracing_dependencies(optional_rhi_features);
        close_bindless_dependencies(required_rhi_features);
        close_bindless_dependencies(optional_rhi_features);
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


        optional_rhi_features.set(RHI::Feature::PresentModeFifoLatestReady);


        optional_rhi_features.set(RHI::Feature::RenderBundles);


        optional_rhi_features.set(RHI::Feature::DepthBoundsTest);


        optional_rhi_features.set(RHI::Feature::ConservativeRasterization);


        optional_rhi_features.set(RHI::Feature::SampleLocations);


        optional_rhi_features.set(RHI::Feature::VariableRateShading);
        optional_rhi_features.set(RHI::Feature::PipelineFragmentShadingRate);
        optional_rhi_features.set(RHI::Feature::AttachmentFragmentShadingRate);


        // OpacityMicromap and RayTracingInvocationReorder are requested inside the
        // `if (init.features.raytracing)` block above instead of here -- both extensions hard-depend
        // on VK_KHR_acceleration_structure, and requesting them independent of raytracing being
        // negotiated on produced an invalid device-extension combination (empirically confirmed via
        // vkCreateDevice failing on UiWorkbench, which doesn't request raytracing).


        optional_rhi_features.set(RHI::Feature::SwapchainMaintenance);
#if defined(_WIN32)


        optional_rhi_features.set(RHI::Feature::ExternalMemory)
            .set(RHI::Feature::ExternalMemoryWin32)
            .set(RHI::Feature::ExternalSemaphore)
            .set(RHI::Feature::ExternalSemaphoreWin32);


        optional_rhi_features.set(RHI::Feature::FullScreenExclusive);
#endif

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
        capabilities_.bindless = enabled_rhi_features.has(RHI::Feature::BindlessResources);


        if (auto resolution = resolve_frames_in_flight(init.features.desired_frames_in_flight, DEFAULT_FRAMES_IN_FLIGHT, 0)) {
            capabilities_.max_frames_in_flight = resolution->resolved;
            TracyPlot("frames_in_flight.requested", static_cast<i64>(resolution->requested));
            TracyPlot("frames_in_flight.lower_bound", static_cast<i64>(resolution->lower_bound));
            TracyPlot("frames_in_flight.resolved", static_cast<i64>(resolution->resolved));
            if (resolution->adjustment != FramesInFlightResolution::Adjustment::Accepted) {
                const bool raised = resolution->adjustment == FramesInFlightResolution::Adjustment::RaisedToLower;
                TracyMessageL(raised ? "frames-in-flight request raised to lower bound"
                                     : "frames-in-flight request reduced to upper bound");
                Foundation::log_info("Frames in flight: requested {} {} to {} (lower_bound={}, upper_bound={}).",
                                     resolution->requested,
                                     raised ? "raised" : "reduced",
                                     resolution->resolved,
                                     resolution->lower_bound,
                                     resolution->upper_bound);
            }
        } else {


            TracyMessageLC("invalid frames-in-flight bounds -- falling back to default", tracy::Color::Red);
            Foundation::log_warn("Frames in flight: {} -- falling back to default ({}).",
                                 resolution.error(),
                                 DEFAULT_FRAMES_IN_FLIGHT);
            capabilities_.max_frames_in_flight = DEFAULT_FRAMES_IN_FLIGHT;
        }

        const bool enable_mesh_shader = enabled_rhi_features.has(RHI::Feature::MeshShader);
        const bool enable_task_shader = enabled_rhi_features.has(RHI::Feature::TaskShader);
        const bool enable_acceleration_structures = enabled_rhi_features.has(RHI::Feature::AccelerationStructures);
        const bool enable_ray_tracing_pipeline = enabled_rhi_features.has(RHI::Feature::RayTracingPipeline);
        const bool enable_ray_query = enabled_rhi_features.has(RHI::Feature::RayQuery);
        const bool enable_bindless_descriptor_heap = enabled_rhi_features.has(RHI::Feature::BindlessResources);
        const bool enable_present_mode_fifo_latest_ready = enabled_rhi_features.has(RHI::Feature::PresentModeFifoLatestReady);
        const bool enable_swapchain_maintenance1 = enabled_rhi_features.has(RHI::Feature::SwapchainMaintenance);
        const bool enable_conservative_rasterization = enabled_rhi_features.has(RHI::Feature::ConservativeRasterization);
        const bool enable_sample_locations = enabled_rhi_features.has(RHI::Feature::SampleLocations);
        const bool enable_pipeline_fragment_shading_rate =
            enabled_rhi_features.has(RHI::Feature::PipelineFragmentShadingRate);
        const bool enable_primitive_fragment_shading_rate =
            enabled_rhi_features.has(RHI::Feature::PrimitiveFragmentShadingRate);
        const bool enable_attachment_fragment_shading_rate =
            enabled_rhi_features.has(RHI::Feature::AttachmentFragmentShadingRate);
        const bool enable_fragment_shading_rate =
            enable_pipeline_fragment_shading_rate || enable_primitive_fragment_shading_rate ||
            enable_attachment_fragment_shading_rate;
        const bool enable_opacity_micromap = enabled_rhi_features.has(RHI::Feature::OpacityMicromap);
        const bool enable_ray_tracing_invocation_reorder =
            enabled_rhi_features.has(RHI::Feature::RayTracingInvocationReorder);
#if defined(_WIN32)
        const bool enable_external_memory_win32 = enabled_rhi_features.has(RHI::Feature::ExternalMemoryWin32);
        const bool enable_external_semaphore_win32 = enabled_rhi_features.has(RHI::Feature::ExternalSemaphoreWin32);
        const bool enable_full_screen_exclusive = enabled_rhi_features.has(RHI::Feature::FullScreenExclusive);
#endif


        VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR presentModeFifoLatestReadyFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
            .pNext = nullptr,
            .presentModeFifoLatestReady = enable_present_mode_fifo_latest_ready ? VK_TRUE : VK_FALSE,
        };
        void *feature_chain_tail = enable_present_mode_fifo_latest_ready
                                       ? static_cast<void *>(&presentModeFifoLatestReadyFeatures)
                                       : nullptr;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR swapchainMaintenance1Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
            .pNext = feature_chain_tail,
            .swapchainMaintenance1 = enable_swapchain_maintenance1 ? VK_TRUE : VK_FALSE,
        };
        if (enable_swapchain_maintenance1) {
            feature_chain_tail = &swapchainMaintenance1Features;
        }
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
            .pNext = feature_chain_tail,
            .rayQuery = enable_ray_query ? VK_TRUE : VK_FALSE,
        };
        if (enable_ray_query) {
            feature_chain_tail = &rayQueryFeatures;
        }
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
            .pNext = feature_chain_tail,
            .rayTracingPipeline = enable_ray_tracing_pipeline ? VK_TRUE : VK_FALSE,
        };
        if (enable_ray_tracing_pipeline) {
            feature_chain_tail = &rayTracingPipelineFeatures;
        }
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            .pNext = feature_chain_tail,
            .accelerationStructure = enable_acceleration_structures ? VK_TRUE : VK_FALSE,
        };
        if (enable_acceleration_structures) {
            feature_chain_tail = &accelerationStructureFeatures;
        }
        VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            .pNext = feature_chain_tail,
            .taskShader = enable_task_shader ? VK_TRUE : VK_FALSE,
            .meshShader = enable_mesh_shader ? VK_TRUE : VK_FALSE,
        };
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR fragmentShadingRateFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
            .pNext = &meshFeatures,
            .pipelineFragmentShadingRate = enable_pipeline_fragment_shading_rate ? VK_TRUE : VK_FALSE,
            .primitiveFragmentShadingRate = enable_primitive_fragment_shading_rate ? VK_TRUE : VK_FALSE,
            .attachmentFragmentShadingRate = enable_attachment_fragment_shading_rate ? VK_TRUE : VK_FALSE,
        };
        VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT invocationReorderFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT,
            .pNext = &fragmentShadingRateFeatures,
            .rayTracingInvocationReorder = enable_ray_tracing_invocation_reorder ? VK_TRUE : VK_FALSE,
        };
        VkPhysicalDeviceOpacityMicromapFeaturesEXT opacityMicromapFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT,
            .pNext = &invocationReorderFeatures,
            .micromap = enable_opacity_micromap ? VK_TRUE : VK_FALSE,
        };


        VkPhysicalDeviceVulkan14Features features14{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
            .pNext = (enable_mesh_shader || enable_acceleration_structures || enable_ray_tracing_pipeline ||
                      enable_ray_query || enable_swapchain_maintenance1 ||
                      enable_present_mode_fifo_latest_ready || enable_fragment_shading_rate ||
                      enable_opacity_micromap || enable_ray_tracing_invocation_reorder)
                         ? &opacityMicromapFeatures
                         : nullptr,
        };
        VkPhysicalDeviceVulkan13Features features13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .pNext = &features14,
            .synchronization2 = VK_TRUE,
            .dynamicRendering = VK_TRUE,
        };
        VkPhysicalDeviceVulkan12Features features12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &features13,
            .descriptorIndexing = enable_bindless_descriptor_heap ? VK_TRUE : VK_FALSE,
            .shaderSampledImageArrayNonUniformIndexing = enable_bindless_descriptor_heap ? VK_TRUE : VK_FALSE,
            .descriptorBindingSampledImageUpdateAfterBind = enable_bindless_descriptor_heap ? VK_TRUE : VK_FALSE,
            .descriptorBindingPartiallyBound = enable_bindless_descriptor_heap ? VK_TRUE : VK_FALSE,
            .descriptorBindingVariableDescriptorCount = enable_bindless_descriptor_heap ? VK_TRUE : VK_FALSE,
            .runtimeDescriptorArray = enable_bindless_descriptor_heap ? VK_TRUE : VK_FALSE,
            .timelineSemaphore = VK_TRUE,
            .bufferDeviceAddress = VK_TRUE,
        };
        VkPhysicalDeviceVulkan11Features features11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext = &features12,
            .shaderDrawParameters = VK_TRUE,
        };
        VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &features11};


        if (supportedFeatures.features.samplerAnisotropy) {
            features.features.samplerAnisotropy = VK_TRUE;
        }


        if (supportedFeatures.features.textureCompressionBC) {
            features.features.textureCompressionBC = VK_TRUE;
        }
        if (enabled_rhi_features.has(RHI::Feature::ImageCubeArray)) {
            features.features.imageCubeArray = VK_TRUE;
        }
        if (enabled_rhi_features.has(RHI::Feature::DepthBoundsTest)) {
            features.features.depthBounds = VK_TRUE;
        }


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


            transfer_family.reset();
        }


        hdr_metadata_enabled_ = false;
        vector<const char *> extensions{
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
        };
        if (enable_mesh_shader) {
            extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        }
        if (enable_acceleration_structures) {
            extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        }
        if (enable_ray_tracing_pipeline) {
            extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        }
        if (enable_ray_query) {
            extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
        }
        if (enable_present_mode_fifo_latest_ready) {
            extensions.push_back(VK_KHR_PRESENT_MODE_FIFO_LATEST_READY_EXTENSION_NAME);
        }
        if (enable_swapchain_maintenance1) {


            extensions.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        }
        if (enable_fragment_shading_rate) {
            extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        }
        if (enable_opacity_micromap) {
            extensions.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
        }
        if (enable_ray_tracing_invocation_reorder) {
            extensions.push_back(VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME);
        }
        if (enable_conservative_rasterization) {
            extensions.push_back(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
        }
        if (enable_sample_locations) {
            extensions.push_back(VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME);
        }
#if defined(_WIN32)
        if (enable_external_memory_win32) {
            extensions.push_back("VK_KHR_external_memory_win32");
        }
        if (enable_external_semaphore_win32) {
            extensions.push_back("VK_KHR_external_semaphore_win32");
        }
        if (enable_full_screen_exclusive) {
            extensions.push_back("VK_EXT_full_screen_exclusive");
        }
#endif
        if (video_decode_family.has_value() || video_encode_family.has_value()) {
            extensions.push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
        }
        if (video_decode_family.has_value()) {
            extensions.push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
        }
        if (video_encode_family.has_value()) {
            extensions.push_back(VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME);
        }


        if (this->physicalDevice.supports_extension(VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
            hdr_metadata_enabled_ = true;
        }


        if (this->physicalDevice.supports_extension(PORTABILITY_SUBSET_EXTENSION_NAME)) {
            extensions.push_back(PORTABILITY_SUBSET_EXTENSION_NAME);
        }


        const u32 graphics_queue_count = preferred_lane_count(this->physicalDevice, gfx_family);
        const u32 present_queue_index = gfx_family.has_value() && present_family.has_value() &&
                                                *gfx_family == *present_family && graphics_queue_count > 1
                                            ? 1u
                                            : 0u;

        VulkanDevice::DeviceCreateDesc desc{
            .graphics_queue_family = gfx_family,
            .present_queue_family = present_family,
            .compute_queue_family = compute_family,
            .transfer_queue_family = transfer_family,
            .sparse_queue_family = sparse_family,
            .video_decode_queue_family = video_decode_family,
            .video_encode_queue_family = video_encode_family,
            .present_queue_index = present_queue_index,
            .graphics_queue_count = graphics_queue_count,
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
            Foundation::log_info("Dedicated Vulkan transfer queue selected: family={} lanes={}", *transfer_family, this->logicalDevice.transfer_queue_lanes().size());
        }
        if (compute_family.has_value()) {
            Foundation::log_info("Vulkan compute queue lanes={}", this->logicalDevice.compute_queue_lanes().size());
        }
        Foundation::log_info("Vulkan graphics queue lanes={}", this->logicalDevice.graphics_queue_lanes().size());
        if (sparse_family.has_value()) {
            Foundation::log_info("Vulkan sparse queue selected: family={} lanes={}", *sparse_family, this->logicalDevice.sparse_queue_lanes().size());
        }
        if (video_decode_family.has_value()) {
            Foundation::log_info("Vulkan video decode queue selected: family={} lanes={}", *video_decode_family, this->logicalDevice.video_decode_queue_lanes().size());
        }
        if (video_encode_family.has_value()) {
            Foundation::log_info("Vulkan video encode queue selected: family={} lanes={}", *video_encode_family, this->logicalDevice.video_encode_queue_lanes().size());
        }
        return {};
    }

    /// Performs the initialize vma operation for `Vulkan` using the supplied arguments.
    ///
    /// @param init `init` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    RendererResult VulkanBackend::initializeVMA(const RendererCreateInfo &init) {
        ZoneScopedN("VulkanBackend::initializeVMA");
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

        // Diagnostic only — see rebar_heap_size_bytes's own doc comment for why nothing here needs to
        // gate or configure anything; VMA already opportunistically uses whatever it finds.
        constexpr VkDeviceSize rebar_heap_threshold_bytes = 512ull * 1024 * 1024; // 512 MiB
        const VkDeviceSize rebar_heap_size = rebar_heap_size_bytes(this->physicalDevice.memory_properties());
        if (rebar_heap_size >= rebar_heap_threshold_bytes) {
            Foundation::log_info(
                "Vulkan Resizable BAR detected: {:.1f} GiB of DEVICE_LOCAL+HOST_VISIBLE memory "
                "available; DeviceLocal buffers will opportunistically skip staging uploads where VMA "
                "places them there.",
                static_cast<f64>(rebar_heap_size) / (1024.0 * 1024.0 * 1024.0));
        } else if (rebar_heap_size > 0) {
            Foundation::log_info(
                "Vulkan DEVICE_LOCAL+HOST_VISIBLE memory available, but only {:.0f} MiB — likely the "
                "legacy pre-Resizable-BAR aperture, not full Resizable BAR; VMA may still "
                "opportunistically use it for small DeviceLocal allocations.",
                static_cast<f64>(rebar_heap_size) / (1024.0 * 1024.0));
        } else {
            Foundation::log_info(
                "Vulkan Resizable BAR is not available on this GPU/driver; DeviceLocal buffers will "
                "always use a staging-buffer upload.");
        }

        Foundation::log_info("VMA Initialization was a success!");
        return {};
    }

} // namespace SFT::Core::Vulkan
