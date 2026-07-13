// Bridge construction, introspection, wait_idle/render_frame, and the shared error-conversion
// helpers every other VulkanRhiBridge*.cpp file relies on. Also VulkanBackend::installRhiBridge()/
// rhi_device() — the two-line glue that used to live in the now-removed flat VulkanBackendRhi.cpp.
module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#pragma endregion

module Sturdy.Core;

import :GraphicsBackendError;
import :VulkanAllocator;
import :VulkanBackend;
import :VulkanCommandPool;
import :VulkanDevice;
import :VulkanNativeAccessExtension;
import :VulkanPhysicalDevice;
import :VulkanQueue;
import :VulkanRhiBridge;
import :VulkanRhiConvert;
import :VulkanSync;
import Sturdy.Foundation;
import Sturdy.RHI;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    namespace {

        [[nodiscard]] u32 queue_lane_count(const std::vector<VulkanQueue> &lanes, VulkanQueue *fallback) noexcept {
            if (!lanes.empty()) {
                return static_cast<u32>(lanes.size());
            }
            return fallback != nullptr && fallback->is_valid() ? 1u : 0u;
        }

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

    VulkanRhiDeviceBridge::VulkanRhiDeviceBridge(VulkanBackend &backend,
                                                 VkInstance instance,
                                                 const VulkanPhysicalDevice &physical_device,
                                                 VulkanDevice &logical_device,
                                                 VulkanQueue &graphics_queue,
                                                 VulkanQueue *compute_queue,
                                                 VulkanQueue *transfer_queue,
                                                 VulkanAllocator &allocator,
                                                 rhi::FeatureNegotiationReport feature_report,
                                                 bool enable_native_access_extension,
                                                 bool hdr_swapchain_colorspace_enabled,
                                                 bool hdr_metadata_enabled)
        : backend_(&backend), instance_(instance), physical_device_(&physical_device),
          logical_device_(&logical_device), graphics_queue_(&graphics_queue), allocator_(&allocator),
          feature_report_(feature_report), enabled_features_(feature_report_.enabled_features()),
          hdr_swapchain_colorspace_enabled_(hdr_swapchain_colorspace_enabled),
          hdr_metadata_enabled_(hdr_metadata_enabled) {
        compute_queue_ = compute_queue;
        transfer_queue_ = transfer_queue;
        if (hdr_swapchain_colorspace_enabled_) {
            enabled_extensions_.push_back(rhi::ExtensionId{"vulkan", "VK_EXT_swapchain_colorspace", 1});
        }
        if (hdr_metadata_enabled_) {
            enabled_extensions_.push_back(rhi::ExtensionId{"vulkan", "VK_EXT_hdr_metadata", 1});
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

        // Best-effort: backs write_buffer()'s staged upload path for DeviceLocal buffers (see
        // VulkanRhiBridgeBuffers.cpp). If either fails to create, upload_via_staging() reports
        // OperationFailed at the point of use rather than here — every other bridge capability is
        // independent of this pool/fence.
        if (auto pool = VulkanCommandPool::create(logical_device_->vk_handle(), graphics_queue_->family_index());
            pool.has_value()) {
            upload_command_pool_ = std::move(*pool);
        }
        if (auto fence = VulkanFence::create(logical_device_->vk_handle()); fence.has_value()) {
            upload_fence_ = std::move(*fence);
        }
    }

    rhi::BackendType VulkanRhiDeviceBridge::backend_type() const noexcept { return rhi::BackendType::Vulkan; }
    const rhi::AdapterInfo &VulkanRhiDeviceBridge::adapter_info() const noexcept { return adapter_info_; }
    const rhi::DeviceLimits &VulkanRhiDeviceBridge::limits() const noexcept { return limits_; }
    const rhi::FeatureNegotiationReport &VulkanRhiDeviceBridge::feature_negotiation_report() const noexcept { return feature_report_; }
    const rhi::FeatureSet &VulkanRhiDeviceBridge::enabled_features() const noexcept { return enabled_features_; }
    const rhi::FeatureProperties &VulkanRhiDeviceBridge::feature_properties() const noexcept { return feature_properties_; }
    std::span<const rhi::QueueInfo> VulkanRhiDeviceBridge::queue_infos() const noexcept { return queue_infos_; }
    std::span<const rhi::ExtensionId> VulkanRhiDeviceBridge::enabled_extensions() const noexcept { return enabled_extensions_; }
    rhi::RhiDeviceExtension *VulkanRhiDeviceBridge::extension_interface(rhi::ExtensionId extension) noexcept {
        if (native_access_extension_.has_value() &&
            rhi::extension_matches(VulkanNativeAccessExtension::id(), extension)) {
            return &*native_access_extension_;
        }
        return nullptr;
    }

    void VulkanRhiDeviceBridge::wait_idle() noexcept {
        if (logical_device_ != nullptr) {
            logical_device_->wait_idle();
        }
    }

    VulkanQueue *VulkanRhiDeviceBridge::queue_for_lane(rhi::QueueLane lane) const noexcept {
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
                return lane_from(logical_device_->compute_queue_lanes(), compute_queue_);
            case rhi::QueueClass::Transfer:
                if (transfer_queue_ == compute_queue_) {
                    return lane_from(logical_device_->compute_queue_lanes(), compute_queue_);
                }
                return lane_from(logical_device_->transfer_queue_lanes(), transfer_queue_);
            case rhi::QueueClass::Sparse:
                return lane_from(logical_device_->sparse_queue_lanes(), logical_device_->sparse_queue().has_value() ? &*logical_device_->sparse_queue() : nullptr);
            case rhi::QueueClass::VideoDecode:
                return lane_from(logical_device_->video_decode_queue_lanes(), logical_device_->video_decode_queue().has_value() ? &*logical_device_->video_decode_queue() : nullptr);
            case rhi::QueueClass::VideoEncode:
                return lane_from(logical_device_->video_encode_queue_lanes(), logical_device_->video_encode_queue().has_value() ? &*logical_device_->video_encode_queue() : nullptr);
        }
        return nullptr;
    }

    u32 VulkanRhiDeviceBridge::queue_family_for_lane(rhi::QueueLane lane) const noexcept {
        if (VulkanQueue *queue = queue_for_lane(lane)) {
            return queue->family_index();
        }
        return VK_QUEUE_FAMILY_IGNORED;
    }

    rhi::RhiResult VulkanRhiDeviceBridge::validate_queue_lane(rhi::QueueLane lane, const char *operation) const {
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

    std::unexpected<rhi::RhiError> VulkanRhiDeviceBridge::rhi_error_from_graphics(const GraphicsBackendError &error) {
        rhi::RhiErrorCode code = rhi::RhiErrorCode::OperationFailed;
        switch (error.code) {
            case GraphicsBackendErrorCode::Unsupported: code = rhi::RhiErrorCode::Unsupported; break;
            case GraphicsBackendErrorCode::OutOfMemory: code = rhi::RhiErrorCode::OutOfMemory; break;
            case GraphicsBackendErrorCode::DeviceLost: code = rhi::RhiErrorCode::DeviceLost; break;
            case GraphicsBackendErrorCode::SurfaceLost: code = rhi::RhiErrorCode::SurfaceLost; break;
            case GraphicsBackendErrorCode::InitializationFailed:
            case GraphicsBackendErrorCode::OperationFailed:
                code = rhi::RhiErrorCode::OperationFailed;
                break;
        }

        return std::unexpected(rhi::RhiError{code, error.message});
    }

    rhi::RhiDevice *VulkanBackend::rhi_device() noexcept {
        return rhiDevice.get();
    }

    const rhi::RhiDevice *VulkanBackend::rhi_device() const noexcept {
        return rhiDevice.get();
    }

    void VulkanBackend::installRhiBridge() {
        auto &device_compute_queue = logicalDevice.compute_queue();
        auto &device_transfer_queue = logicalDevice.transfer_queue();
        VulkanQueue *compute_queue = device_compute_queue.has_value() ? &*device_compute_queue : nullptr;
        VulkanQueue *transfer_queue = device_transfer_queue.has_value() ? &*device_transfer_queue : nullptr;
        if (transfer_queue == nullptr && feature_report_.enabled_features().has(rhi::Feature::AsyncTransfer) &&
            compute_queue != nullptr && compute_queue->family_index() != gfxQueue.family_index()) {
            transfer_queue = compute_queue;
        }
        // Startup installs the bridge once. Existing module/BMI layout issues can leave this unique_ptr
        // containing a garbage pre-assignment value, so avoid deleting that value before first ownership.
        static_cast<void>(rhiDevice.release());
        rhiDevice = std::make_unique<VulkanRhiDeviceBridge>(*this, vulkan_instance, physicalDevice, logicalDevice, gfxQueue,
                                                            compute_queue, transfer_queue, vmaAllocator, feature_report_,
                                                            static_cast<bool>(create_info_.features.enable_native_access_extension),
                                                            hdr_swapchain_colorspace_enabled_, hdr_metadata_enabled_);
    }

} // namespace SFT::Core::Vulkan
