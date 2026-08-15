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
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <vector>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

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
        ZoneScopedN("VulkanBackend::findPhysicalDevice");
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
        ZoneScopedN("VulkanBackend::discoverGraphicsQueue");
        (void)init;
        if (auto res = this->physicalDevice.findGraphicsQueue(primary_surface); !res.has_value()) [[unlikely]] {
            return graphics_backend_error(GraphicsBackendErrorCode::InitializationFailed, "Your GPU is apparently not Vulkan Compliant!! the Vulkan spec guarantees one graphics queue and we found zero");
        }
        Foundation::log_info("Successfully got a graphics queue from the physical device!");
        return {};
    }

    RendererResult VulkanBackend::createDevice(const RendererCreateInfo &init, VkSurfaceKHR primary_surface) {
        ZoneScopedN("VulkanBackend::createDevice");
        (void)init;

        // Query which features the physical device actually supports.
        // VK_KHR_present_mode_fifo_latest_ready has no accompanying queue/backend integration work
        // beyond this — it's a pure presentation capability, so it's queried/enabled unconditionally
        // here rather than needing its own opt-in path (see RHI::Feature::PresentModeFifoLatestReady's
        // own doc comment, and the "Optional Core: enabled when present" handling below).
        VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR supportedPresentModeFifoLatestReadyFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,
            .pNext = nullptr};
        // Detection/enablement only for now -- RHI::Feature::SwapchainMaintenance gates whether
        // VkSwapchainPresentFenceInfoEXT/vkReleaseSwapchainImagesEXT are *legal* to use, but nothing
        // in this codebase calls them yet (present-fence-gated swapchain retirement, replacing the
        // wait_idle() fallback in Renderer::maybe_flush_retired_swapchains, is tracked as follow-up
        // work — see that function's own doc comment). Querying it now so
        // RHI::RhiDevice::enabled_features() correctly reports availability regardless.
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR supportedSwapchainMaintenance1Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
            .pNext = &supportedPresentModeFifoLatestReadyFeatures,
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
        // No VkPhysicalDeviceXXXFeatures struct to check for either of these — both extensions add
        // structs/functions, not a VkBool32 feature bit, so extension support is the whole story.
        // Named as plain literals (not the VK_KHR_EXTERNAL_..._WIN32_EXTENSION_NAME macros) because
        // those live in vulkan_win32.h, gated behind VK_USE_PLATFORM_WIN32_KHR — not worth pulling
        // windows.h into this cross-platform file just for two string constants; the real Win32
        // interop work that does need those headers lives in VulkanRhiBridgeComposition.cpp.
        if (this->physicalDevice.supports_extension("VK_KHR_external_memory_win32")) {
            supported_rhi_features.set(RHI::Feature::ExternalMemory).set(RHI::Feature::ExternalMemoryWin32);
        }
        if (this->physicalDevice.supports_extension("VK_KHR_external_semaphore_win32")) {
            supported_rhi_features.set(RHI::Feature::ExternalSemaphore).set(RHI::Feature::ExternalSemaphoreWin32);
        }
        // VK_EXT_full_screen_exclusive is entirely Win32-specific in the Vulkan headers (even its own
        // extension-name macro lives in vulkan_win32.h, not vulkan_core.h) — same reason as the two
        // extensions above for naming it as a plain literal instead of the macro, and requires its
        // instance-level dependency (surface_capabilities2_enabled_, VulkanBackendInstance.cpp) too.
        if (surface_capabilities2_enabled_ &&
            this->physicalDevice.supports_extension("VK_EXT_full_screen_exclusive")) {
            supported_rhi_features.set(RHI::Feature::FullScreenExclusive);
        }
#endif
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
            // Some GPUs expose a distinct async compute family that also supports transfer, but no pure
            // DMA/copy family. It is still useful for RHI Transfer work because it is distinct from graphics.
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
        // Optional Core: enabled whenever the device reports it, never gated behind an app request
        // — a lower-latency Fifo variant with no tradeoff for an app to weigh, unlike raytracing/
        // async-compute/etc. above (all real capacity/power tradeoffs the app opts into deliberately).
        optional_rhi_features.set(RHI::Feature::PresentModeFifoLatestReady);
        // Same reasoning as PresentModeFifoLatestReady immediately above: a pure capability with no
        // tradeoff, enabled whenever the device reports it. Detection/enablement only for now — see
        // the query-time struct's own doc comment above for what's not wired up yet.
        optional_rhi_features.set(RHI::Feature::SwapchainMaintenance);
#if defined(_WIN32)
        // Same opportunistic-enable reasoning again: enabling these extensions changes nothing about
        // default rendering, so there's no tradeoff for an app to weigh. They back the composition-
        // present fallback (VulkanRhiBridgeComposition.cpp) that keeps a transparent window working on
        // Win32/Vulkan surfaces whose composite alpha only supports Opaque — see
        // resolve_composite_alpha's own doc comment (VulkanRhiBridgeSwapchain.cpp) for when that path
        // is actually needed.
        optional_rhi_features.set(RHI::Feature::ExternalMemory)
            .set(RHI::Feature::ExternalMemoryWin32)
            .set(RHI::Feature::ExternalSemaphore)
            .set(RHI::Feature::ExternalSemaphoreWin32);
        // Same reasoning once more: enabling the extension by itself changes nothing — exclusive mode
        // is only ever entered when a swapchain explicitly requests it
        // (RHI::SwapchainDesc::request_full_screen_exclusive), which only happens when the window is
        // actually in WindowMode::ExclusiveFullscreen.
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
        // DEFAULT_FRAMES_IN_FLIGHT as the lower bound (not 1) preserves today's "0 means double-
        // buffer" behavior exactly, while still routing through the one centralized picker every
        // frames-in-flight-derived subsystem now consumes RendererCapabilities::max_frames_in_flight
        // from (see resolve_frames_in_flight's doc comment, Core/Renderer.hpp). upper_bound is 0
        // (unbounded) here — a real surface-derived upper bound isn't wired in yet.
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
            // Unreachable today (upper_bound is always 0/unbounded here, so lower_bound can never
            // exceed it) — kept as a real fallback rather than an assert so a future caller that does
            // pass a real upper bound can't turn an invalid-range configuration error into UB.
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
#if defined(_WIN32)
        const bool enable_external_memory_win32 = enabled_rhi_features.has(RHI::Feature::ExternalMemoryWin32);
        const bool enable_external_semaphore_win32 = enabled_rhi_features.has(RHI::Feature::ExternalSemaphoreWin32);
        const bool enable_full_screen_exclusive = enabled_rhi_features.has(RHI::Feature::FullScreenExclusive);
#endif

        // Build the enable chain — only request what we verified above.
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
        // meshFeatures must stay chained here whenever *any* struct behind it in the chain
        // (rayQueryFeatures / rayTracingPipelineFeatures / accelerationStructureFeatures /
        // swapchainMaintenance1Features / presentModeFifoLatestReadyFeatures) is needed — dropping it
        // based on enable_mesh_shader alone would silently disconnect all of those from the enable
        // chain too whenever mesh shading itself is off. Every bool that conditionally advances
        // feature_chain_tail above must be listed here.
        VkPhysicalDeviceVulkan14Features features14{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
            .pNext = (enable_mesh_shader || enable_acceleration_structures || enable_ray_tracing_pipeline ||
                      enable_ray_query || enable_swapchain_maintenance1 ||
                      enable_present_mode_fifo_latest_ready)
                         ? &meshFeatures
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
        // Anisotropic filtering: enable whenever the device supports it, so the RHI sampler path can
        // honor SamplerDesc::max_anisotropy (clamped to the device limit at create_sampler time).
        // Near-universal on real GPUs; guarded because portability-subset implementations may lack it.
        if (supportedFeatures.features.samplerAnisotropy) {
            features.features.samplerAnisotropy = VK_TRUE;
        }
        // BC1-7 texture compression: core Vulkan 1.0 feature (not a vendor extension), enabled
        // whenever supported — near-universal on desktop GPUs. Lets Engine::AssetManager upload
        // Format::BC7Unorm/etc. textures (see RHI::DeviceLimits::supports_bc_texture_compression,
        // populated from this same query in VulkanRhiBridgeCore.cpp).
        if (supportedFeatures.features.textureCompressionBC) {
            features.features.textureCompressionBC = VK_TRUE;
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
            // The feature struct in the device pNext chain is only valid when its owning extension
            // is enabled too. This also makes the RHI feature report truthful for the future
            // present-fence retirement path.
            extensions.push_back(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
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
        // HDR metadata is optional, but enable it up front whenever available. Device extensions are
        // immutable after creation; pre-enabling this avoids replacing the device—and invalidating
        // GPU-only scene assets—when an SDR application later switches its swapchain to HDR.
        if (this->physicalDevice.supports_extension(VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
            hdr_metadata_enabled_ = true;
        }

        // The Vulkan spec requires enabling VK_KHR_portability_subset on any device that
        // advertises it — MoltenVK and other non-conformant implementations use it to report
        // which core features they can't fully provide. Omitting it despite support is invalid.
        if (this->physicalDevice.supports_extension(PORTABILITY_SUBSET_EXTENSION_NAME)) {
            extensions.push_back(PORTABILITY_SUBSET_EXTENSION_NAME);
        }

        // A second queue from the graphics family is the best portable latency win available when
        // the driver exposes one: vkQueuePresentKHR can block in the WSI, but it then blocks only the
        // presentation queue while graphics submissions keep flowing on queue 0. A separate family is
        // deliberately not selected here because that needs explicit swapchain-image ownership transfer;
        // the existing graphics queue remains the safe fallback in that topology.
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

        Foundation::log_info("VMA Initialization was a success!");
        return {};
    }

} // namespace SFT::Core::Vulkan
