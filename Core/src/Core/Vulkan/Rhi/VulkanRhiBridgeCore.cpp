

#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <algorithm>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

#include <Foundation/src/Foundation.hpp>

#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/Rhi/VulkanNativeAccessExtension.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridge.hpp>
#include <Core/Vulkan/VulkanRhiConvert.hpp>
#include <RHI/RHI.hpp>

#include <tracy/Tracy.hpp>

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        /// Returns the queue lane count for this `Vulkan`.
        ///
        /// @param lanes `lanes` value used by the operation.
        /// @param fallback Fallback value used when the primary value is unavailable.
        ///
        /// @return Returns the requested count or size.
        /// @note This function does not throw exceptions.
        [[nodiscard]] u32 queue_lane_count(const std::vector<VulkanQueue> &lanes, VulkanQueue *fallback) noexcept {
            if (!lanes.empty()) {
                return static_cast<u32>(lanes.size());
            }
            return fallback != nullptr && fallback->is_valid() ? 1u : 0u;
        }

        /// Returns a human-readable name for the supplied queue class value.
        ///
        /// @param queue Queue used or affected by the operation.
        ///
        /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
        /// @note This function does not throw exceptions.
        [[nodiscard]] constexpr std::string_view queue_class_name(rhi::QueueClass queue) noexcept {
            switch (queue) {
                case rhi::QueueClass::Graphics: return "Graphics";
                case rhi::QueueClass::Compute: return "Compute";
                case rhi::QueueClass::Transfer: return "Transfer";
                case rhi::QueueClass::Sparse: return "Sparse";
                case rhi::QueueClass::VideoDecode: return "VideoDecode";
                case rhi::QueueClass::VideoEncode: return "VideoEncode";
            }
            return "Unknown";
        }


    } // namespace

    /// Performs the vulkan RHI device bridge operation for `Vulkan` using the supplied arguments.
    ///
    /// @param backend Backend value to inspect, select, or convert.
    /// @param instance Instance used or affected by the operation.
    /// @param physical_device Device used or affected by the operation.
    /// @param logical_device Device used or affected by the operation.
    /// @param graphics_queue Queue used or affected by the operation.
    /// @param present_queue Queue used or affected by the operation.
    /// @param compute_queue Queue used or affected by the operation.
    /// @param transfer_queue Queue used or affected by the operation.
    /// @param allocator Allocator used for storage owned by the operation.
    /// @param feature_report `feature_report` value used by the operation.
    /// @param enable_native_access_extension Whether the associated behavior is enabled.
    /// @param hdr_swapchain_colorspace_enabled Swapchain used or affected by the operation.
    /// @param hdr_metadata_enabled `hdr_metadata_enabled` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    VulkanRhiDeviceBridge::VulkanRhiDeviceBridge(VulkanBackend &backend,
                                                 VkInstance instance,
                                                 const VulkanPhysicalDevice &physical_device,
                                                 VulkanDevice &logical_device,
                                                 VulkanQueue &graphics_queue,
                                                 VulkanQueue &present_queue,
                                                 VulkanQueue *compute_queue,
                                                 VulkanQueue *transfer_queue,
                                                 VulkanAllocator &allocator,
                                                 rhi::FeatureNegotiationReport feature_report,
                                                 bool enable_native_access_extension,
                                                 bool hdr_swapchain_colorspace_enabled,
                                                 bool hdr_metadata_enabled)
        : backend_(&backend), instance_(instance), physical_device_(&physical_device),
          logical_device_(&logical_device), graphics_queue_(&graphics_queue), present_queue_(&present_queue),
          allocator_(&allocator),
          feature_report_(feature_report), enabled_features_(feature_report_.enabled_features()),
          hdr_swapchain_colorspace_enabled_(hdr_swapchain_colorspace_enabled),
          hdr_metadata_enabled_(hdr_metadata_enabled) {
        ZoneScopedN("VulkanRhiDeviceBridge::VulkanRhiDeviceBridge");
        compute_queue_ = compute_queue;
        transfer_queue_ = transfer_queue;
        if (hdr_swapchain_colorspace_enabled_) {
            enabled_extensions_.push_back(rhi::ExtensionId{"vulkan", "VK_EXT_swapchain_colorspace", 1});
        }
        if (hdr_metadata_enabled_) {
            enabled_extensions_.push_back(rhi::ExtensionId{"vulkan", "VK_EXT_hdr_metadata", 1});
        }
        if (enabled_features_.has(rhi::Feature::SwapchainMaintenance)) {
            enabled_extensions_.push_back(rhi::ExtensionId{"vulkan", "VK_KHR_swapchain_maintenance1", 1});
        }
        if (enable_native_access_extension) {
            native_access_extension_.emplace(
                instance_, physical_device_->vk_handle(), logical_device_->vk_handle(), graphics_queue_->vk_handle(), this,
                [](void *context, rhi::QueueLane lane) noexcept -> VkQueue {
                    auto *bridge = static_cast<VulkanRhiDeviceBridge *>(context);
                    VulkanQueue *queue = bridge != nullptr ? bridge->queue_for_lane(lane) : nullptr;
                    return queue != nullptr ? queue->vk_handle() : VK_NULL_HANDLE;
                },
                [](void *context, rhi::QueueLane lane) noexcept -> u32 {
                    auto *bridge = static_cast<VulkanRhiDeviceBridge *>(context);
                    return bridge != nullptr ? bridge->queue_family_for_lane(lane) : VK_QUEUE_FAMILY_IGNORED;
                });
            enabled_extensions_.push_back(VulkanNativeAccessExtension::id());
        }
        const VkPhysicalDeviceProperties &props = physical_device.properties();
        const VkPhysicalDeviceLimits &limits = props.limits;

        adapter_info_.name = physical_device.name().cpp_string();
        adapter_info_.vendor = physical_device.vendor_name();
        adapter_info_.driver_version = physical_device.driver_version_string().cpp_string();
        adapter_info_.api_version = physical_device.api_version_string().cpp_string();
        adapter_info_.backend = rhi::BackendType::Vulkan;
        adapter_info_.device_type = to_rhi_device_type(props.deviceType);
        adapter_info_.vendor_id = props.vendorID;
        adapter_info_.device_id = props.deviceID;
        adapter_info_.is_discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

        limits_.max_texture_dimension_2d = limits.maxImageDimension2D;
        limits_.max_texture_array_layers = limits.maxImageArrayLayers;
        limits_.max_bind_groups = limits.maxBoundDescriptorSets;
        limits_.max_push_constants_size = limits.maxPushConstantsSize;
        limits_.max_vertex_buffers = limits.maxVertexInputBindings;
        limits_.max_vertex_attributes = limits.maxVertexInputAttributes;
        limits_.max_color_attachments = limits.maxColorAttachments;
        const VkSampleCountFlags framebuffer_samples =
            limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
        limits_.framebuffer_sample_counts = framebuffer_samples;
        limits_.max_framebuffer_sample_count =
            (framebuffer_samples & VK_SAMPLE_COUNT_16_BIT) != 0 ? 16u :
            (framebuffer_samples & VK_SAMPLE_COUNT_8_BIT) != 0 ? 8u :
            (framebuffer_samples & VK_SAMPLE_COUNT_4_BIT) != 0 ? 4u :
            (framebuffer_samples & VK_SAMPLE_COUNT_2_BIT) != 0 ? 2u : 1u;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
        };
        VkPhysicalDeviceDescriptorIndexingProperties descriptor_indexing_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,
            .pNext = &acceleration_structure_properties,
        };
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,
            .pNext = &descriptor_indexing_properties,
        };
        VkPhysicalDeviceDepthStencilResolveProperties depth_resolve_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,
            .pNext = &ray_tracing_properties,
        };
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragment_shading_rate_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR,
            .pNext = &depth_resolve_properties,
        };
        VkPhysicalDeviceProperties2 extended_properties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &fragment_shading_rate_properties,
        };
        vkGetPhysicalDeviceProperties2(physical_device.vk_handle(), &extended_properties);
        limits_.supports_minimum_depth_resolve =
            (depth_resolve_properties.supportedDepthResolveModes & VK_RESOLVE_MODE_MIN_BIT) != 0;
        if (enabled_features_.has(rhi::Feature::RayTracingPipeline)) {
            feature_properties_.ray_tracing.max_ray_recursion_depth = ray_tracing_properties.maxRayRecursionDepth;
            feature_properties_.ray_tracing.shader_group_handle_size = ray_tracing_properties.shaderGroupHandleSize;
            feature_properties_.ray_tracing.shader_group_base_alignment = ray_tracing_properties.shaderGroupBaseAlignment;
            feature_properties_.ray_tracing.max_ray_hit_attribute_size = ray_tracing_properties.maxRayHitAttributeSize;
        }
        if (enabled_features_.has(rhi::Feature::AccelerationStructures)) {
            feature_properties_.ray_tracing.max_acceleration_structure_geometry_count =
                static_cast<u32>(acceleration_structure_properties.maxGeometryCount);
            feature_properties_.ray_tracing.max_acceleration_structure_instance_count =
                static_cast<u32>(acceleration_structure_properties.maxInstanceCount);
            feature_properties_.ray_tracing.min_acceleration_structure_scratch_offset_alignment =
                acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
        }
        if (enabled_features_.has(rhi::Feature::AttachmentFragmentShadingRate)) {
            // Vulkan reports a legal range (min..max) for the shading-rate attachment's texel size;
            // D3D12's equivalent (D3D12_FEATURE_DATA_D3D12_OPTIONS6::ShadingRateImageTileSize) is a
            // single fixed value with no such range. Pinning both min and max to Vulkan's own
            // minimum here gives every caller one concrete, always-legal tile size on both backends
            // instead of a Vulkan-only range a D3D12-targeting caller would have no use for.
            feature_properties_.variable_rate_shading.min_tile_width =
                fragment_shading_rate_properties.minFragmentShadingRateAttachmentTexelSize.width;
            feature_properties_.variable_rate_shading.min_tile_height =
                fragment_shading_rate_properties.minFragmentShadingRateAttachmentTexelSize.height;
            feature_properties_.variable_rate_shading.max_tile_width =
                fragment_shading_rate_properties.minFragmentShadingRateAttachmentTexelSize.width;
            feature_properties_.variable_rate_shading.max_tile_height =
                fragment_shading_rate_properties.minFragmentShadingRateAttachmentTexelSize.height;
        }
        if (enabled_features_.has(rhi::Feature::BindlessResources)) {
            feature_properties_.descriptor_indexing.max_update_after_bind_descriptors =
                descriptor_indexing_properties.maxUpdateAfterBindDescriptorsInAllPools;
            feature_properties_.descriptor_indexing.max_variable_descriptor_count = std::min(
                descriptor_indexing_properties.maxDescriptorSetUpdateAfterBindSamplers,
                descriptor_indexing_properties.maxDescriptorSetUpdateAfterBindSampledImages);
        }
        limits_.supports_bc_texture_compression = physical_device.features().textureCompressionBC == VK_TRUE;
        limits_.max_compute_workgroup_size_x = limits.maxComputeWorkGroupSize[0];
        limits_.max_compute_workgroup_size_y = limits.maxComputeWorkGroupSize[1];
        limits_.max_compute_workgroup_size_z = limits.maxComputeWorkGroupSize[2];
        limits_.min_uniform_buffer_offset_alignment = limits.minUniformBufferOffsetAlignment;
        limits_.min_storage_buffer_offset_alignment = limits.minStorageBufferOffsetAlignment;
        limits_.timestamp_period_ns = limits.timestampPeriod;
        limits_.timestamp_valid_bits = physical_device.timestamp_valid_bits(graphics_queue.family_index());

        queue_infos_.push_back(rhi::QueueInfo{
            .queue = rhi::QueueClass::Graphics,
            .capabilities = rhi::QueueCapability::Graphics | rhi::QueueCapability::Compute |
                            rhi::QueueCapability::Transfer | rhi::QueueCapability::Present,
            .lane_count = queue_lane_count(logical_device_->graphics_queue_lanes(), graphics_queue_),
            .physical_group = graphics_queue_->family_index(),
            .likely_parallel_with_graphics = false,
            .dedicated = false,
            .label = "Vulkan graphics/present queue",
        });
        if (compute_queue_ != nullptr && compute_queue_->is_valid() &&
            compute_queue_->family_index() != graphics_queue_->family_index()) {
            queue_infos_.push_back(rhi::QueueInfo{
                .queue = rhi::QueueClass::Compute,
                .capabilities = rhi::QueueCapability::Compute | rhi::QueueCapability::Transfer,
                .lane_count = queue_lane_count(logical_device_->compute_queue_lanes(), compute_queue_),
                .physical_group = compute_queue_->family_index(),
                .likely_parallel_with_graphics = true,
                .dedicated = true,
                .label = "Vulkan dedicated compute queue",
            });
        }
        if (transfer_queue_ != nullptr && transfer_queue_->is_valid() &&
            transfer_queue_->family_index() != graphics_queue_->family_index()) {
            const bool aliases_compute = compute_queue_ != nullptr && transfer_queue_ == compute_queue_;
            queue_infos_.push_back(rhi::QueueInfo{
                .queue = rhi::QueueClass::Transfer,
                .capabilities = aliases_compute
                    ? (rhi::QueueCapability::Compute | rhi::QueueCapability::Transfer)
                    : rhi::QueueCapability::Transfer,
                .lane_count = aliases_compute
                    ? queue_lane_count(logical_device_->compute_queue_lanes(), compute_queue_)
                    : queue_lane_count(logical_device_->transfer_queue_lanes(), transfer_queue_),
                .physical_group = transfer_queue_->family_index(),
                .likely_parallel_with_graphics = true,
                .dedicated = true,
                .label = aliases_compute ? "Vulkan transfer lane aliasing dedicated compute queue" : "Vulkan dedicated transfer queue",
            });
        }
        if (auto &sparse = logical_device_->sparse_queue(); sparse.has_value() && sparse->is_valid()) {
            queue_infos_.push_back(rhi::QueueInfo{
                .queue = rhi::QueueClass::Sparse,
                .capabilities = rhi::QueueCapability::SparseBinding,
                .lane_count = queue_lane_count(logical_device_->sparse_queue_lanes(), &*sparse),
                .physical_group = sparse->family_index(),
                .likely_parallel_with_graphics = sparse->family_index() != graphics_queue_->family_index(),
                .dedicated = sparse->family_index() != graphics_queue_->family_index(),
                .label = "Vulkan sparse binding queue",
            });
        }
        if (auto &decode = logical_device_->video_decode_queue(); decode.has_value() && decode->is_valid()) {
            queue_infos_.push_back(rhi::QueueInfo{
                .queue = rhi::QueueClass::VideoDecode,
                .capabilities = rhi::QueueCapability::VideoDecode,
                .lane_count = queue_lane_count(logical_device_->video_decode_queue_lanes(), &*decode),
                .physical_group = decode->family_index(),
                .likely_parallel_with_graphics = decode->family_index() != graphics_queue_->family_index(),
                .dedicated = decode->family_index() != graphics_queue_->family_index(),
                .label = "Vulkan video decode queue",
            });
        }
        if (auto &encode = logical_device_->video_encode_queue(); encode.has_value() && encode->is_valid()) {
            queue_infos_.push_back(rhi::QueueInfo{
                .queue = rhi::QueueClass::VideoEncode,
                .capabilities = rhi::QueueCapability::VideoEncode,
                .lane_count = queue_lane_count(logical_device_->video_encode_queue_lanes(), &*encode),
                .physical_group = encode->family_index(),
                .likely_parallel_with_graphics = encode->family_index() != graphics_queue_->family_index(),
                .dedicated = encode->family_index() != graphics_queue_->family_index(),
                .label = "Vulkan video encode queue",
            });
        }


        if (auto cache = VulkanPipelineCache::create(logical_device_->vk_handle(), {})) {
            pipeline_cache_ = std::move(*cache);
        }
    }

    /// Destroys the `Vulkan` and releases resources owned by it.
    ///
    /// @note Destruction does not return a failure status; resource-release failures are handled by the operations performed during teardown.
    VulkanRhiDeviceBridge::~VulkanRhiDeviceBridge() {
        ZoneScopedN("VulkanRhiDeviceBridge::~VulkanRhiDeviceBridge");


        wait_idle();
        if (logical_device_ != nullptr) {
            shader_modules_.drain([this](VkShaderModule module) noexcept {
                if (module != VK_NULL_HANDLE) {
                    logical_device_->destroy_shader_module(module);
                }
            });
        } else {
            shader_modules_.drain([](VkShaderModule) noexcept {});
        }
    }

    /// Returns the current or globally available backend type value.
    ///
    /// @return Returns the current backend type value.
    /// @note This function does not throw exceptions.
    rhi::BackendType VulkanRhiDeviceBridge::backend_type() const noexcept { return rhi::BackendType::Vulkan; }
    /// Returns the current adapter info.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::AdapterInfo &VulkanRhiDeviceBridge::adapter_info() const noexcept { return adapter_info_; }
    /// Returns the current or globally available limits value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::DeviceLimits &VulkanRhiDeviceBridge::limits() const noexcept { return limits_; }
    /// Returns the current or globally available feature negotiation report value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::FeatureNegotiationReport &VulkanRhiDeviceBridge::feature_negotiation_report() const noexcept { return feature_report_; }
    /// Returns the current or globally available enabled features value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::FeatureSet &VulkanRhiDeviceBridge::enabled_features() const noexcept { return enabled_features_; }
    /// Returns the current or globally available feature properties value.
    ///
    /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
    /// @note This function does not throw exceptions.
    const rhi::FeatureProperties &VulkanRhiDeviceBridge::feature_properties() const noexcept { return feature_properties_; }
    /// Returns the current or globally available queue infos value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const rhi::QueueInfo> VulkanRhiDeviceBridge::queue_infos() const noexcept { return queue_infos_; }
    /// Returns the current or globally available enabled extensions value.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
    std::span<const rhi::ExtensionId> VulkanRhiDeviceBridge::enabled_extensions() const noexcept { return enabled_extensions_; }
    /// Performs the extension interface operation for `Vulkan` using the supplied arguments.
    ///
    /// @param extension `extension` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    rhi::RhiDeviceExtension *VulkanRhiDeviceBridge::extension_interface(rhi::ExtensionId extension) noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::extension_interface");
        if (native_access_extension_.has_value() &&
            rhi::extension_matches(VulkanNativeAccessExtension::id(), extension)) {
            return &*native_access_extension_;
        }
        return nullptr;
    }

    /// Waits for idle to complete.
    ///
    /// @return Returns the current wait idle value.
    /// @note This function does not throw exceptions.
    void VulkanRhiDeviceBridge::wait_idle() noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::wait_idle");
        if (logical_device_ != nullptr) {
            logical_device_->wait_idle();
        }
    }

    /// Performs the queue for lane operation for `Vulkan` using the supplied arguments.
    ///
    /// @param lane `lane` value used by the operation.
    ///
    /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
    /// @note Absence is represented by a null pointer rather than an exception.
    /// @note This function does not throw exceptions.
    VulkanQueue *VulkanRhiDeviceBridge::queue_for_lane(rhi::QueueLane lane) const noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::queue_for_lane");
        auto lane_from = [index = lane.index](std::vector<VulkanQueue> &lanes, VulkanQueue *fallback) noexcept -> VulkanQueue * {
            if (!lanes.empty()) {
                return index < lanes.size() ? &lanes[index] : nullptr;
            }
            return index == 0 && fallback != nullptr && fallback->is_valid() ? fallback : nullptr;
        };


        switch (lane.queue) {
            case rhi::QueueClass::Graphics:
                return lane.index == 0 ? graphics_queue_ : lane_from(logical_device_->graphics_queue_lanes(), nullptr);
            case rhi::QueueClass::Compute:
                return lane.index == 0 ? compute_queue_ : lane_from(logical_device_->compute_queue_lanes(), nullptr);
            case rhi::QueueClass::Transfer:
                if (transfer_queue_ == compute_queue_) {
                    return lane.index == 0 ? compute_queue_ : lane_from(logical_device_->compute_queue_lanes(), nullptr);
                }
                return lane.index == 0 ? transfer_queue_ : lane_from(logical_device_->transfer_queue_lanes(), nullptr);
            case rhi::QueueClass::Sparse: {
                VulkanQueue *primary = logical_device_->sparse_queue().has_value() ? &*logical_device_->sparse_queue() : nullptr;
                return lane.index == 0 ? primary : lane_from(logical_device_->sparse_queue_lanes(), nullptr);
            }
            case rhi::QueueClass::VideoDecode: {
                VulkanQueue *primary = logical_device_->video_decode_queue().has_value() ? &*logical_device_->video_decode_queue() : nullptr;
                return lane.index == 0 ? primary : lane_from(logical_device_->video_decode_queue_lanes(), nullptr);
            }
            case rhi::QueueClass::VideoEncode: {
                VulkanQueue *primary = logical_device_->video_encode_queue().has_value() ? &*logical_device_->video_encode_queue() : nullptr;
                return lane.index == 0 ? primary : lane_from(logical_device_->video_encode_queue_lanes(), nullptr);
            }
        }
        return nullptr;
    }

    /// Performs the queue family for lane operation for `Vulkan` using the supplied arguments.
    ///
    /// @param lane `lane` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    u32 VulkanRhiDeviceBridge::queue_family_for_lane(rhi::QueueLane lane) const noexcept {
        ZoneScopedN("VulkanRhiDeviceBridge::queue_family_for_lane");
        if (VulkanQueue *queue = queue_for_lane(lane)) {
            return queue->family_index();
        }
        return VK_QUEUE_FAMILY_IGNORED;
    }

    /// Validates queue lane.
    ///
    /// @param lane `lane` value used by the operation.
    /// @param operation `operation` value used by the operation.
    ///
    /// @return Returns the successful result/status when the operation completes; the type-specific error state describes a failure.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`.
    rhi::RhiResult VulkanRhiDeviceBridge::validate_queue_lane(rhi::QueueLane lane, const char *operation) const {
        ZoneScopedN("VulkanRhiDeviceBridge::validate_queue_lane");
        if (queue_for_lane(lane) != nullptr) {
            return {};
        }

        std::string message = operation ? operation : "Vulkan RHI queue operation";
        message += ": requested queue lane ";
        message += queue_class_name(lane.queue);
        message += "[";
        message += std::to_string(lane.index);
        message += "] is not exposed by this device. Inspect RhiDevice::queue_infos() and require Feature::AsyncCompute/AsyncTransfer only when true async queues are available.";
        return rhi::rhi_error(rhi::RhiErrorCode::InvalidArgument, std::move(message));
    }

    /// Performs the RHI error from graphics operation for `Vulkan` using the supplied arguments.
    ///
    /// @param error Error value describing the failure.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::OperationFailed`, `GraphicsBackendErrorCode::Unsupported`, `RhiErrorCode::Unsupported`, `GraphicsBackendErrorCode::OutOfMemory`, `RhiErrorCode::OutOfMemory`, `GraphicsBackendErrorCode::DeviceLost` among others.
    std::unexpected<rhi::RhiError> VulkanRhiDeviceBridge::rhi_error_from_graphics(const GraphicsBackendError &error) {
        ZoneScopedN("VulkanRhiDeviceBridge::rhi_error_from_graphics");
        rhi::RhiErrorCode code = rhi::RhiErrorCode::OperationFailed;
        switch (error.code) {
            case GraphicsBackendErrorCode::Unsupported: code = rhi::RhiErrorCode::Unsupported; break;
            case GraphicsBackendErrorCode::OutOfMemory: code = rhi::RhiErrorCode::OutOfMemory; break;
            case GraphicsBackendErrorCode::DeviceLost: code = rhi::RhiErrorCode::DeviceLost; break;
            case GraphicsBackendErrorCode::SurfaceLost: code = rhi::RhiErrorCode::SurfaceLost; break;
            case GraphicsBackendErrorCode::FullScreenExclusiveLost: code = rhi::RhiErrorCode::FullScreenExclusiveLost; break;
            case GraphicsBackendErrorCode::InitializationFailed:
            case GraphicsBackendErrorCode::OperationFailed:
                code = rhi::RhiErrorCode::OperationFailed;
                break;
        }

        return std::unexpected(rhi::RhiError{code, error.message});
    }

    /// Returns the current or globally available RHI device value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    rhi::RhiDevice *VulkanBackend::rhi_device() noexcept {
        ZoneScopedN("VulkanBackend::rhi_device");
        return rhiDevice.get();
    }

    /// Returns the current or globally available RHI device value.
    ///
    /// @return Returns a pointer to the requested object/resource; ownership is not transferred unless the API explicitly states otherwise.
    /// @note This function does not throw exceptions.
    const rhi::RhiDevice *VulkanBackend::rhi_device() const noexcept {
        ZoneScopedN("VulkanBackend::rhi_device");
        return rhiDevice.get();
    }

    /// Returns the current or globally available install RHI bridge value.
    ///
    /// @return Returns the current install RHI bridge value.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void VulkanBackend::installRhiBridge() {
        ZoneScopedN("VulkanBackend::installRhiBridge");
        auto &device_present_queue = logicalDevice.present_queue();
        auto &device_compute_queue = logicalDevice.compute_queue();
        auto &device_transfer_queue = logicalDevice.transfer_queue();


        VulkanQueue *present_queue = &gfxQueue;
        if (device_present_queue.has_value() &&
            device_present_queue->family_index() == gfxQueue.family_index() &&
            device_present_queue->vk_handle() != gfxQueue.vk_handle()) {
            present_queue = &*device_present_queue;
        }
        VulkanQueue *compute_queue = device_compute_queue.has_value() ? &*device_compute_queue : nullptr;
        VulkanQueue *transfer_queue = device_transfer_queue.has_value() ? &*device_transfer_queue : nullptr;
        if (transfer_queue == nullptr && feature_report_.enabled_features().has(rhi::Feature::AsyncTransfer) &&
            compute_queue != nullptr && compute_queue->family_index() != gfxQueue.family_index()) {
            transfer_queue = compute_queue;
        }


        [[maybe_unused]] RHI::RhiDevice *discarded_bridge = rhiDevice.release();
        rhiDevice = std::make_unique<VulkanRhiDeviceBridge>(*this, vulkan_instance, physicalDevice, logicalDevice, gfxQueue,
                                                            *present_queue, compute_queue, transfer_queue, vmaAllocator, feature_report_,
                                                            static_cast<bool>(create_info_.features.enable_native_access_extension),
                                                            hdr_swapchain_colorspace_enabled_, hdr_metadata_enabled_);
    }

} // namespace SFT::Core::Vulkan
