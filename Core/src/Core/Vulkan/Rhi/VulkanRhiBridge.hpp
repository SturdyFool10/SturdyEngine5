#pragma once

#include <Foundation/src/Foundation.hpp>
#pragma region Imports
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#endif
#include "volk.h"
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Async/src/Mutex.hpp>
#include <Core/GraphicsBackendError.hpp>
#include <Core/Vulkan/VulkanAccelerationStructure.hpp>
#include <Core/Vulkan/VulkanAllocator.hpp>
#include <Core/Vulkan/VulkanBackend.hpp>
#include <Core/Vulkan/VulkanBuffer.hpp>
#include <Core/Vulkan/VulkanCommandBuffer.hpp>
#include <Core/Vulkan/VulkanCommandPool.hpp>
#include <Core/Vulkan/VulkanDescriptors.hpp>
#include <Core/Vulkan/VulkanDevice.hpp>
#include <Core/Vulkan/VulkanImage.hpp>
#include <Core/Vulkan/VulkanPhysicalDevice.hpp>
#include <Core/Vulkan/VulkanPipeline.hpp>
#include <Core/Vulkan/Rhi/VulkanNativeAccessExtension.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridgeComposition.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiBridgeFullScreenExclusive.hpp>
#include <Core/Vulkan/VulkanQueryPool.hpp>
#include <Core/Vulkan/VulkanQueue.hpp>
#include <Core/Vulkan/Rhi/VulkanRhiResourcePool.hpp>
#include <Core/Vulkan/VulkanSampler.hpp>
#include <Core/Vulkan/VulkanSync.hpp>
#include <RHI/RHI.hpp>

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
                              VulkanQueue &present_queue,
                              VulkanQueue *compute_queue,
                              VulkanQueue *transfer_queue,
                              VulkanAllocator &allocator,
                              rhi::FeatureNegotiationReport feature_report,
                              bool enable_native_access_extension = false,
                              bool hdr_swapchain_colorspace_enabled = false,
                              bool hdr_metadata_enabled = false);

        ~VulkanRhiDeviceBridge() override;

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
        // `desc` carries the native (system, display, window) identity used later for
        // query_hdr_capabilities() — the surface was already created by a windowing library
        // (SDL/GLFW) when this is called, so it can't be recovered from the raw VkSurfaceKHR alone.
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHandle> import_surface(VkSurfaceKHR surface, const rhi::SurfaceDesc &desc);
        void destroy_surface(rhi::SurfaceHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SwapchainHandle> create_swapchain(const rhi::SwapchainDesc &desc) override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            rhi::SurfaceHandle handle) const override;
        [[nodiscard]] rhi::RhiResult update_hdr_content_light_level(
            rhi::SwapchainHandle handle, const rhi::HdrContentLightLevelUpdate &update) override;
        void destroy_swapchain(rhi::SwapchainHandle handle) noexcept override;
        [[nodiscard]] rhi::PresentationResolution presentation_resolution(rhi::SwapchainHandle handle) const noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceTexture> acquire_next_texture(rhi::SwapchainHandle swapchain, u32 frame_slot_index) override;
        [[nodiscard]] rhi::RhiExpected<rhi::PresentOutcome> present(const rhi::PresentDesc &desc, f64 *queue_lock_wait_ms = nullptr) override;

        [[nodiscard]] rhi::RhiExpected<rhi::SemaphoreHandle> create_semaphore(const rhi::SemaphoreDesc &desc) override;
        void destroy_semaphore(rhi::SemaphoreHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<u64> semaphore_value(rhi::SemaphoreHandle handle) const override;
        [[nodiscard]] rhi::RhiResult wait_semaphore(rhi::SemaphoreHandle handle, u64 value, u64 timeout_ns = rhi::wait_forever) override;
        [[nodiscard]] rhi::RhiResult signal_semaphore(rhi::SemaphoreHandle handle, u64 value) override;
        [[nodiscard]] rhi::RhiExpected<rhi::FenceHandle> create_fence(const rhi::FenceDesc &desc) override;
        void destroy_fence(rhi::FenceHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<bool> wait_fences(span<const rhi::FenceHandle> fences, bool wait_all = true, u64 timeout_ns = rhi::wait_forever) override;
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

        // Backs the common (non-bindless) create_bind_group() path: a pool sized generously for many
        // sets, shared across however many ordinary bind groups fit in it, instead of one dedicated
        // vkCreateDescriptorPool per set. See create_bind_group()'s doc comment in the .cpp for why.
        struct DescriptorPoolChunk {
            VulkanDescriptorPool pool;
            u32 live_sets = 0;
        };

        // Most bind groups are allocated from a shared DescriptorPoolChunk (shared_chunk_index >= 0,
        // `pool` below left default/empty) — see create_bind_group(). Update-after-bind/variable-
        // descriptor-count sets can't share a chunk that way (different pool creation requirements,
        // and are rare/bindless-shaped in practice), so those still get a dedicated private pool sized
        // to exactly their one set (shared_chunk_index stays -1).
        struct BindGroupRecord {
            VulkanDescriptorPool pool;
            VkDescriptorSet set = VK_NULL_HANDLE;
            i32 shared_chunk_index = -1;
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

        // One one-shot command pool/fence pair backing a single upload_via_staging() call — bundled so
        // a single Async::Mutex-guarded free-list (upload_pool_ below) can check entries in/out as a
        // unit instead of a bare std::mutex alongside two loose members.
        struct UploadResources {
            VulkanCommandPool command_pool;
            VulkanFence fence;
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
            // Native (system, display, window) identity, kept for query_hdr_capabilities() — display/
            // window are non-owning (the owning Window/VulkanSurface outlives this record; a surface
            // never survives its window closing). `label` is copied so query_hdr_capabilities() never
            // reads through a caller's possibly-freed const char* — desc_.label always points at
            // stored_label below, never at the original caller's pointer.
            rhi::SurfaceDesc desc{};
            std::string stored_label{};
        };

        // Builds a SurfaceRecord that owns a copy of `desc`'s label rather than the caller's raw
        // pointer (the returned record's desc.label always points at its own stored_label).
        [[nodiscard]] static SurfaceRecord make_surface_record(VkSurfaceKHR surface, bool owns_surface,
                                                                const rhi::SurfaceDesc &desc);

        struct SwapchainRecord {
            VulkanSwapchain swapchain;
            rhi::SurfaceHandle surface{};
            vector<rhi::TextureHandle> textures;
            vector<rhi::TextureViewHandle> views;
            vector<VulkanSemaphore> image_available_semaphores;
            vector<u32> image_available_signal_indices;
            vector<VulkanSemaphore> render_finished_semaphores;
            u32 current_image = ~0u;
            bool current_suboptimal = false;
            // Requested-vs-effective presentation state (RHI::PresentationResolution's own doc
            // comment) — set once at create_swapchain() time, read back via
            // VulkanRhiDeviceBridge::presentation_resolution() (e.g. the Renderer debug overlay).
            rhi::PresentationResolution presentation_resolution{};
            // Mirrors presentation_resolution.present_queue_is_compute — kept as its own bool (not just
            // re-read from presentation_resolution) so present() can check it without reasoning about
            // the rest of that struct; set once at create_swapchain() time alongside it.
            bool present_via_compute = false;
            // The VkHdrMetadataEXT last sent via vkSetHdrMetadataEXT (create_swapchain() sends the
            // first one for an Hdr10St2084 swapchain), retained so
            // update_hdr_content_light_level() can resend it with just maxContentLightLevel/
            // maxFrameAverageLightLevel overwritten — the display's real primaries/white-point/
            // luminance range shouldn't need re-querying on every scene-light-level update.
            VkHdrMetadataEXT stored_hdr_metadata{};
            bool has_hdr_metadata = false;
            // Non-null exactly when this swapchain presents through the OS compositor instead of
            // vkQueuePresentKHR. On Windows this is attempted for every generation, including opaque
            // ones, so enabling transparency later does not layer a DirectComposition visual over the
            // final image retained by a native WSI swapchain. When set, `textures`/`views` above hold
            // the imported composition images instead of native swapchain images, and `swapchain`
            // itself is left default-constructed/invalid — every other field on this record
            // (image_available_semaphores, render_finished_semaphores, presentation_resolution, ...)
            // is populated exactly as it would be for a native swapchain, so only
            // acquire_next_texture()/present()/destroy_swapchain() need to branch on this being set.
            CompositionSwapchainResources composition{};
            // 0 = present without waiting for vblank, 1 = wait for vblank — DXGI's own Present()
            // meaning, resolved once at create_swapchain() time from the negotiated present mode
            // (composition present has no per-present concept of "which mode" the way
            // vkQueuePresentKHR does, so this is the closest equivalent this record can carry).
            u32 composition_sync_interval = 1;
            // Format/alpha-mode this record's composition presenter was actually built with — both are
            // fixed at DXGI swap-chain creation and can't change through CompositionPresenter::resize.
            // create_swapchain() compares these against a fresh request to decide whether an in-place
            // resize (reusing the presenter, and critically its already-attached DirectComposition
            // target — see resize_composition_swapchain_resources's own doc comment,
            // VulkanRhiBridgeComposition.hpp, for why that reuse matters) is even possible, or whether
            // a full rebuild is unavoidable because the format or transparency request itself changed.
            VkFormat composition_vk_format = VK_FORMAT_UNDEFINED;
            GraphicsPlatform::CompositionAlphaMode composition_alpha_mode =
                GraphicsPlatform::CompositionAlphaMode::Ignore;
            // True once this record ever became composition-present, and stays true even after
            // composition.presenter is released early (see create_swapchain()'s own handling of
            // desc.old_swapchain for why that happens: DirectComposition supports only one
            // IDCompositionTarget per HWND at a time, so a retiring composition-present record has its
            // presenter torn down synchronously — detaching that target from the window — the moment a
            // replacement swapchain for the same surface starts being built, rather than waiting for
            // the normal several-frames-later deferred retirement the rest of this record's resources
            // still go through). destroy_swapchain() checks *this*, not is_composition_present(), to
            // decide whether destroy_composition_swapchain_resources() still needs to run for the
            // memory/semaphores that early release deliberately left behind.
            bool owns_composition_resources = false;
            // True exactly when acquire_full_screen_exclusive_mode() actually succeeded for this
            // swapchain — release_full_screen_exclusive_mode() must only be called when this is true
            // (see its own doc comment, VulkanRhiBridgeFullScreenExclusive.hpp, for why releasing an
            // exclusive mode that was never acquired is undefined per spec rather than a safe no-op).
            // Mirrored into presentation_resolution.full_screen_exclusive_active for callers, but kept
            // as its own bool too so destroy_swapchain() can check it without reasoning about the rest
            // of that struct — same rationale as present_via_compute above.
            bool full_screen_exclusive_active = false;

            [[nodiscard]] bool is_composition_present() const noexcept {
                return composition.presenter != nullptr;
            }
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

        // Checks one UploadResources entry out of upload_pool_ (reusing a returned one if the free-list
        // is non-empty, else creating a fresh pool/fence pair) so concurrent upload_via_staging() calls
        // each get their own command pool/fence instead of serializing on one shared pair. A pool/fence
        // creation failure is returned as an invalid (default-constructed-looking) entry — the caller
        // checks validity and reports OperationFailed rather than this function itself failing, matching
        // upload_via_staging's existing best-effort-resource-creation contract.
        [[nodiscard]] UploadResources acquire_upload_resources();
        // Returns `resources` to upload_pool_ for reuse by a later call. A broken entry (pool/fence
        // creation failed in acquire_upload_resources) is dropped instead of recycled — pushing an
        // invalid entry back would just hand the same failure to the next caller.
        void release_upload_resources(UploadResources resources) noexcept;

        // Checks one (pool, command buffer) pair out of command_buffer_free_list_ for `family_index`
        // (std::nullopt if that family's free list is empty — the caller falls back to creating a
        // fresh pair). See command_buffer_free_list_'s own doc comment.
        [[nodiscard]] std::optional<CommandBufferRecord> checkout_command_buffer(u32 family_index) noexcept;
        // Resets `record`'s pool (recycling its one command buffer back to initial state) and returns
        // it to command_buffer_free_list_ for reuse, keyed by the pool's own family_index. A pool
        // reset failure (rare) drops the entry instead of returning a possibly-corrupt pool to the
        // free list — it is simply destroyed here (record goes out of scope), same fallback
        // release_upload_resources uses for a broken entry.
        void return_command_buffer(CommandBufferRecord &&record) noexcept;

        VulkanBackend *backend_ = nullptr;
        VkInstance instance_ = VK_NULL_HANDLE;
        const VulkanPhysicalDevice *physical_device_ = nullptr;
        VulkanDevice *logical_device_ = nullptr;
        VulkanQueue *graphics_queue_ = nullptr;
        // May alias graphics_queue_ when the device exposes only one queue or presentation lives in a
        // different family that would require ownership transfers. Otherwise it is queue 1 from the
        // same graphics family and keeps a WSI-blocked present from serializing graphics submission.
        VulkanQueue *present_queue_ = nullptr;
        VulkanAllocator *allocator_ = nullptr;

        // In-process-only cache. Persisted driver blobs are intentionally disabled because malformed
        // or crash-produced AMDVLK data can fault inside pipeline creation instead of being rejected.
        VulkanPipelineCache pipeline_cache_{};
        // Vulkan requires external synchronization for every operation that uses one VkPipelineCache,
        // including graphics/compute/ray-tracing pipeline creation. Renderer initialization can create
        // pipelines from multiple worker tasks, so the cache cannot be shared without this lock.
        Async::Mutex<u8> pipeline_cache_mutex_{0};

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
        // Grows on demand (a new chunk whenever the last one's allocation fails) and never shrinks —
        // chunks are cheap to keep once paid for, and re-creating them every time usage dipped to zero
        // would reintroduce the exact per-call vkCreateDescriptorPool cost this exists to avoid.
        // Declared after bind_groups_ so it's destroyed after (every BindGroupRecord referencing a
        // chunk index must be gone, or at least its destruction must not need to touch these pools,
        // before the pools themselves go away).
        //
        // Unlike bind_groups_ (a VulkanRhiResourcePool, internally mutex-guarded), this vector and the
        // vkAllocateDescriptorSets/vkFreeDescriptorSets calls against its pools need their own guard:
        // create_bind_group()/destroy_bind_group() are documented as safe to call concurrently from
        // multiple windows' render calls, and the Vulkan spec requires external synchronization per
        // VkDescriptorPool. Async::Mutex<T> rather than a bare std::mutex + vector so the vector is
        // simply unreachable without holding the lock — nothing to accidentally get wrong.
        Async::Mutex<vector<DescriptorPoolChunk>> descriptor_pool_chunks_;
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

        // Free-list of reusable upload command pool/fence pairs, guarded only across the brief
        // pop_back()/push_back() checkout — the actual record/submit/wait/reset work in
        // upload_via_staging() runs on a caller's own checked-out entry, outside this lock, so
        // concurrent uploads on different entries genuinely overlap instead of fully serializing.
        // Grows lazily to whatever the real peak concurrent-upload count turns out to be, then stays
        // that size (entries are always returned, never destroyed, once created).
        Async::Mutex<vector<UploadResources>> upload_pool_;

        // Same shape as upload_pool_ immediately above, generalized to every create_command_encoder()
        // caller instead of just upload_via_staging(): a per-queue-family free list of reusable
        // (pool, command buffer) pairs, keyed by VulkanQueue::family_index() since a command pool is
        // only ever compatible with the family it was created for. create_command_encoder() checks
        // one out (falling back to a fresh vkCreateCommandPool/vkAllocateCommandBuffers only when the
        // family's free list is empty); destroy_command_buffer() resets and returns it instead of
        // destroying it. Grows lazily to the real peak concurrent-encoder count per family, then
        // stays that size. See checkout_command_buffer()/return_command_buffer() (VulkanRhiBridgeCommands.cpp).
        Async::Mutex<std::unordered_map<u32, vector<CommandBufferRecord>>> command_buffer_free_list_;

        // Appended after the original bridge state to avoid shifting resource-pool offsets across module
        // implementation units while the project is still using fragile C++23 module/BMI generation.
        VulkanQueue *compute_queue_ = nullptr;
        VulkanQueue *transfer_queue_ = nullptr;
        // Populated only when the app opted in (RendererFeatureRequest::enable_native_access_extension);
        // returned by extension_interface() when the caller requests VulkanNativeAccessExtension::id().
        std::optional<VulkanNativeAccessExtension> native_access_extension_;
    };

} // namespace SFT::Core::Vulkan
