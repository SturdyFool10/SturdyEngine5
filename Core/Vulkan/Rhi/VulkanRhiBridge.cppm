module;
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <cstddef>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#pragma endregion

export module Sturdy.Core:VulkanRhiBridge;

import :GraphicsBackendError;
import :VulkanAccelerationStructure;
import :VulkanAllocator;
import :VulkanBackend;
import :VulkanBuffer;
import :VulkanCommandBuffer;
import :VulkanCommandPool;
import :VulkanDescriptors;
import :VulkanDevice;
import :VulkanImage;
import :VulkanPhysicalDevice;
import :VulkanPipeline;
import :VulkanNativeAccessExtension;
import :VulkanQueryPool;
import :VulkanQueue;
import :VulkanRhiResourcePool;
import :VulkanSampler;
import :VulkanSync;
import Sturdy.Foundation;
import Sturdy.RHI;

using std::span;
using std::unique_ptr;
using std::vector;

namespace SFT::Core::Vulkan {

    namespace rhi = SFT::RHI;

    // Sturdy.RHI's implementation of the live Vulkan backend. Every `create_*`/`destroy_*` mints or
    // releases a handle in one of the resource pools below. Method bodies are split by concern
    // across VulkanRhiBridge*.cpp, mirroring how VulkanBackend's own implementation is split.
    class VulkanRhiDeviceBridge final : public rhi::RhiDevice {
      public:
        VulkanRhiDeviceBridge(VulkanBackend &backend,
                              VkInstance instance,
                              const VulkanPhysicalDevice &physical_device,
                              VulkanDevice &logical_device,
                              VulkanQueue &graphics_queue,
                              VulkanQueue *compute_queue,
                              VulkanQueue *transfer_queue,
                              VulkanAllocator &allocator,
                              rhi::FeatureNegotiationReport feature_report,
                              bool enable_native_access_extension = false,
                              bool hdr_swapchain_colorspace_enabled = false,
                              bool hdr_metadata_enabled = false);

        // ── Introspection ──
        [[nodiscard]] rhi::BackendType backend_type() const noexcept override;
        [[nodiscard]] const rhi::AdapterInfo &adapter_info() const noexcept override;
        [[nodiscard]] const rhi::DeviceLimits &limits() const noexcept override;
        [[nodiscard]] const rhi::FeatureNegotiationReport &feature_negotiation_report() const noexcept override;
        [[nodiscard]] const rhi::FeatureSet &enabled_features() const noexcept override;
        [[nodiscard]] const rhi::FeatureProperties &feature_properties() const noexcept override;
        [[nodiscard]] span<const rhi::QueueInfo> queue_infos() const noexcept override;
        [[nodiscard]] span<const rhi::ExtensionId> enabled_extensions() const noexcept override;
        [[nodiscard]] rhi::RhiDeviceExtension *extension_interface(rhi::ExtensionId extension) noexcept override;

        // ── Resources (Phase 1) ──
        [[nodiscard]] rhi::RhiExpected<rhi::BufferHandle> create_buffer(const rhi::BufferDesc &desc) override;
        void destroy_buffer(rhi::BufferHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::TextureHandle> create_texture(const rhi::TextureDesc &desc) override;
        void destroy_texture(rhi::TextureHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::TextureViewHandle> create_texture_view(const rhi::TextureViewDesc &desc) override;
        void destroy_texture_view(rhi::TextureViewHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SamplerHandle> create_sampler(const rhi::SamplerDesc &desc) override;
        void destroy_sampler(rhi::SamplerHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::ShaderModuleHandle> create_shader_module(const rhi::ShaderModuleDesc &desc) override;
        void destroy_shader_module(rhi::ShaderModuleHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::BindGroupLayoutHandle> create_bind_group_layout(const rhi::BindGroupLayoutDesc &desc) override;
        void destroy_bind_group_layout(rhi::BindGroupLayoutHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::BindGroupHandle> create_bind_group(const rhi::BindGroupDesc &desc) override;
        void destroy_bind_group(rhi::BindGroupHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::PipelineLayoutHandle> create_pipeline_layout(const rhi::PipelineLayoutDesc &desc) override;
        void destroy_pipeline_layout(rhi::PipelineLayoutHandle handle) noexcept override;

        // ── Pipelines (Phase 2) ──
        [[nodiscard]] rhi::RhiExpected<rhi::RenderPipelineHandle> create_render_pipeline(const rhi::RenderPipelineDesc &desc) override;
        void destroy_render_pipeline(rhi::RenderPipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::ComputePipelineHandle> create_compute_pipeline(const rhi::ComputePipelineDesc &desc) override;
        void destroy_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::RayTracingPipelineHandle> create_ray_tracing_pipeline(
            const rhi::RayTracingPipelineDesc &desc) override;
        void destroy_ray_tracing_pipeline(rhi::RayTracingPipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult write_ray_tracing_shader_group_handles(
            rhi::RayTracingPipelineHandle pipeline, u32 first_group, u32 group_count, span<std::byte> dst) override;

        // ── Acceleration structures / device addresses (Phase 7) ──
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> acceleration_structure_build_sizes(
            const rhi::AccelerationStructureBuildDesc &desc) const override;
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureHandle> create_acceleration_structure(
            const rhi::AccelerationStructureDesc &desc) override;
        void destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<u64> buffer_device_address(rhi::BufferHandle buffer) const override;
        [[nodiscard]] rhi::RhiExpected<u64> acceleration_structure_device_address(
            rhi::AccelerationStructureHandle handle) const override;

        // ── Data upload (Phase 1) ──
        [[nodiscard]] rhi::RhiResult write_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) override;
        [[nodiscard]] rhi::RhiExpected<span<std::byte>> map_buffer(rhi::BufferHandle buffer) override;
        void unmap_buffer(rhi::BufferHandle buffer) noexcept override;

        // ── Command recording / submission / presentation / sync / queries ──
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> create_command_encoder(const rhi::CommandEncoderDesc &desc) override;
        void destroy_command_buffer(rhi::CommandBufferHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> create_render_bundle_encoder(const rhi::RenderBundleDesc &desc) override;
        void destroy_render_bundle(rhi::RenderBundleHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult submit(const rhi::SubmitDesc &desc) override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHandle> create_surface(const rhi::SurfaceDesc &desc) override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHandle> import_surface(VkSurfaceKHR surface);
        void destroy_surface(rhi::SurfaceHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SwapchainHandle> create_swapchain(const rhi::SwapchainDesc &desc) override;
        void destroy_swapchain(rhi::SwapchainHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceTexture> acquire_next_texture(rhi::SwapchainHandle swapchain) override;
        [[nodiscard]] rhi::RhiExpected<bool> present(const rhi::PresentDesc &desc) override;

        [[nodiscard]] rhi::RhiExpected<rhi::SemaphoreHandle> create_semaphore(const rhi::SemaphoreDesc &desc) override;
        void destroy_semaphore(rhi::SemaphoreHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<u64> semaphore_value(rhi::SemaphoreHandle handle) const override;
        [[nodiscard]] rhi::RhiResult wait_semaphore(rhi::SemaphoreHandle handle, u64 value, u64 timeout_ns = rhi::wait_forever) override;
        [[nodiscard]] rhi::RhiResult signal_semaphore(rhi::SemaphoreHandle handle, u64 value) override;
        [[nodiscard]] rhi::RhiExpected<rhi::FenceHandle> create_fence(const rhi::FenceDesc &desc) override;
        void destroy_fence(rhi::FenceHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult wait_fences(span<const rhi::FenceHandle> fences, bool wait_all = true, u64 timeout_ns = rhi::wait_forever) override;
        [[nodiscard]] rhi::RhiResult reset_fences(span<const rhi::FenceHandle> fences) override;
        [[nodiscard]] rhi::RhiExpected<rhi::QuerySetHandle> create_query_set(const rhi::QuerySetDesc &desc) override;
        void destroy_query_set(rhi::QuerySetHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult get_query_set_results(rhi::QuerySetHandle query_set, u32 first, u32 count,
                                                            span<std::byte> dst, u64 stride,
                                                            rhi::QueryResultFlags flags = rhi::QueryResultFlags::Result64Bit) override;
        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) noexcept override;

        void wait_idle() noexcept override;

      private:
        // Owns exactly the resources of one buffer's memory intent, so write_buffer/map_buffer can
        // decide whether to stage-and-copy (DeviceLocal) or map directly (HostUpload/HostReadback)
        // without re-deriving it from Vulkan-level flags.
        struct BufferRecord {
            VulkanBuffer buffer;
            rhi::MemoryLocation memory = rhi::MemoryLocation::DeviceLocal;
        };

        // Keeps the originating RHI format next to the image so create_texture_view can compute the
        // right VkImageAspectFlags when TextureViewDesc::format is Undefined ("inherit"; the common
        // case) — VulkanImage only stores the VkFormat, which doesn't carry enough to reverse-map.
        struct TextureRecord {
            VulkanImage image;
            rhi::Format format = rhi::Format::Undefined;
        };

        // The layout plus its own entry list — create_bind_group needs the entries again (binding
        // type per slot) to size a pool and translate BindGroupEntry writes, and VulkanDescriptorSetLayout
        // itself doesn't retain them.
        struct BindGroupLayoutRecord {
            VulkanDescriptorSetLayout layout;
            vector<rhi::BindGroupLayoutEntry> entries;
        };

        // Each bind group owns a private pool sized to exactly its one set. Simple and correct; a
        // shared growable pool allocator is a later optimization once bind-group churn matters.
        struct BindGroupRecord {
            VulkanDescriptorPool pool;
            VkDescriptorSet set = VK_NULL_HANDLE;
        };

        struct PipelineRecord {
            VulkanPipeline pipeline;
            rhi::PipelineLayoutHandle layout{};
        };

        struct CommandBufferRecord {
            VulkanCommandPool pool;
            VulkanCommandBuffer command_buffer;
            rhi::QueueLane queue{};
        };

        struct RenderBundleRecord {
            VulkanCommandPool pool;
            VulkanCommandBuffer command_buffer;
        };

        struct AccelerationStructureRecord {
            VulkanBuffer backing_buffer;
            VulkanAccelerationStructure acceleration_structure;
        };

        struct SurfaceRecord {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            bool owns_surface = true;
        };

        struct SwapchainRecord {
            VulkanSwapchain swapchain;
            rhi::SurfaceHandle surface{};
            vector<rhi::TextureHandle> textures;
            vector<rhi::TextureViewHandle> views;
            vector<VulkanSemaphore> image_available_semaphores;
            vector<u32> image_available_signal_indices;
            vector<VulkanSemaphore> render_finished_semaphores;
            u32 acquire_cursor = 0;
            u32 current_image = ~0u;
            bool current_suboptimal = false;
        };

        friend VkAccelerationStructureGeometryKHR to_vk_geometry(
            const rhi::AccelerationStructureGeometryDesc &geometry,
            const VulkanRhiDeviceBridge &bridge) noexcept;
        friend class VulkanRhiEncoderCommon;
        friend class VulkanRhiCommandEncoder;
        friend class VulkanRhiRenderPassEncoder;
        friend class VulkanRhiComputePassEncoder;
        friend class VulkanRhiRenderBundleEncoder;

        // Template — must stay defined here (not split into a .cpp) so every translation unit that
        // calls it, across all the VulkanRhiBridge*.cpp files, sees a definition to instantiate.
        template <typename Value>
        [[nodiscard]] static rhi::RhiExpected<Value> device_not_ready(const char *operation) {
            return rhi::rhi_error(rhi::RhiErrorCode::OperationFailed,
                                  std::string("Vulkan RHI bridge cannot run ") + operation + ": device resources are not ready.");
        }
        [[nodiscard]] static std::unexpected<rhi::RhiError> rhi_error_from_graphics(const GraphicsBackendError &error);
        [[nodiscard]] VulkanQueue *queue_for_lane(rhi::QueueLane lane) const noexcept;
        [[nodiscard]] u32 queue_family_for_lane(rhi::QueueLane lane) const noexcept;
        [[nodiscard]] rhi::RhiResult validate_queue_lane(rhi::QueueLane lane, const char *operation) const;

        // Stages `data` into a transient host-visible buffer, then blocking-copies it into
        // `destination` at `offset` via a one-shot command buffer on the graphics queue. The backend's
        // only transfer path until real command recording lands (Phase 3) — write_buffer's documented
        // behavior for a DeviceLocal destination requires exactly this, so it can't wait for that phase.
        [[nodiscard]] rhi::RhiResult upload_via_staging(VulkanBuffer &destination, u64 offset, span<const std::byte> data);

        VulkanBackend *backend_ = nullptr;
        VkInstance instance_ = VK_NULL_HANDLE;
        const VulkanPhysicalDevice *physical_device_ = nullptr;
        VulkanDevice *logical_device_ = nullptr;
        VulkanQueue *graphics_queue_ = nullptr;
        VulkanAllocator *allocator_ = nullptr;

        rhi::AdapterInfo adapter_info_{};
        rhi::DeviceLimits limits_{};
        rhi::FeatureNegotiationReport feature_report_{};
        rhi::FeatureSet enabled_features_{};
        rhi::FeatureProperties feature_properties_{};
        vector<rhi::QueueInfo> queue_infos_{};
        vector<rhi::ExtensionId> enabled_extensions_{};
        bool hdr_swapchain_colorspace_enabled_ = false;
        bool hdr_metadata_enabled_ = false;

        VulkanRhiResourcePool<rhi::BufferHandle, BufferRecord> buffers_;
        VulkanRhiResourcePool<rhi::TextureHandle, TextureRecord> textures_;
        VulkanRhiResourcePool<rhi::TextureViewHandle, VulkanImageView> texture_views_;
        VulkanRhiResourcePool<rhi::SamplerHandle, VulkanSampler> samplers_;
        VulkanRhiResourcePool<rhi::ShaderModuleHandle, VkShaderModule> shader_modules_;
        VulkanRhiResourcePool<rhi::BindGroupLayoutHandle, BindGroupLayoutRecord> bind_group_layouts_;
        VulkanRhiResourcePool<rhi::BindGroupHandle, BindGroupRecord> bind_groups_;
        VulkanRhiResourcePool<rhi::PipelineLayoutHandle, VulkanPipelineLayout> pipeline_layouts_;
        VulkanRhiResourcePool<rhi::RenderPipelineHandle, PipelineRecord> render_pipelines_;
        VulkanRhiResourcePool<rhi::ComputePipelineHandle, PipelineRecord> compute_pipelines_;
        VulkanRhiResourcePool<rhi::RayTracingPipelineHandle, PipelineRecord> ray_tracing_pipelines_;
        VulkanRhiResourcePool<rhi::AccelerationStructureHandle, AccelerationStructureRecord> acceleration_structures_;
        VulkanRhiResourcePool<rhi::CommandBufferHandle, CommandBufferRecord> command_buffers_;
        VulkanRhiResourcePool<rhi::RenderBundleHandle, RenderBundleRecord> render_bundles_;
        VulkanRhiResourcePool<rhi::SemaphoreHandle, VulkanSemaphore> semaphores_;
        VulkanRhiResourcePool<rhi::FenceHandle, VulkanFence> fences_;
        VulkanRhiResourcePool<rhi::QuerySetHandle, VulkanQueryPool> query_sets_;
        VulkanRhiResourcePool<rhi::SurfaceHandle, SurfaceRecord> surfaces_;
        VulkanRhiResourcePool<rhi::SwapchainHandle, SwapchainRecord> swapchains_;

        VulkanCommandPool upload_command_pool_;
        VulkanFence upload_fence_;
        std::mutex upload_mutex_;

        // Appended after the original bridge state to avoid shifting resource-pool offsets across module
        // implementation units while the project is still using fragile C++23 module/BMI generation.
        VulkanQueue *compute_queue_ = nullptr;
        VulkanQueue *transfer_queue_ = nullptr;
        // Populated only when the app opted in (RendererFeatureRequest::enable_native_access_extension);
        // returned by extension_interface() when the caller requests VulkanNativeAccessExtension::id().
        std::optional<VulkanNativeAccessExtension> native_access_extension_;
    };

} // namespace SFT::Core::Vulkan
