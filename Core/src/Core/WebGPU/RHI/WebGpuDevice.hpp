#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#pragma endregion

#include <RHI/RHI.hpp>
#include <Core/WebGPU/RHI/WebGpuCommon.hpp>

namespace SFT::Core::WebGpu {

    namespace rhi = ::SFT::RHI;

    using std::span;
    using std::unique_ptr;
    using std::vector;

    /// The WebGPU-backed `RhiDevice`.
    ///
    /// Sits on Dawn, which in this build talks only to Vulkan, Metal, or D3D12 (see
    /// `sturdy_fetch_dawn` in cmake/SturdyDependencies.cmake). That restriction is what makes this
    /// backend worth having rather than a curiosity: it is a second, independent path onto the same
    /// three modern drivers the native backends use, so a bug that reproduces here but not on the
    /// native Vulkan backend is almost certainly in this engine's own usage rather than in a driver.
    ///
    /// WebGPU is deliberately a smaller API than Vulkan or D3D12, and several parts of this
    /// engine's RHI have no counterpart in it at all — ray tracing and acceleration structures,
    /// mesh/task and geometry/tessellation stages, push constants, timeline semaphores, buffer
    /// device addresses, multi-draw indirect, and depth bounds. Every one of those returns an
    /// explicit "not supported by WebGPU" error naming the feature, rather than being emulated or
    /// silently ignored; a caller that hits one needs to know it is a property of the API and not a
    /// gap that a later patch fills in.
    class WebGpuDevice final : public rhi::RhiDevice {
      public:
        /// Constructs a device around an already-created Dawn device.
        ///
        /// @param instance The Dawn instance the device came from; retained for surface creation
        ///        and for the future-waiting the async entry points need.
        /// @param device The Dawn device, whose ownership passes to this object.
        /// @param info Adapter description reported upward.
        /// @param limits Device limits reported upward.
        /// @param features Features negotiated at creation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        WebGpuDevice(WGPUInstance instance, WGPUDevice device, rhi::AdapterInfo info,
                     rhi::DeviceLimits limits, rhi::FeatureSet features);

        /// Destroys the `WebGpuDevice` and releases every WebGPU object it still owns.
        ///
        /// @note This function does not throw exceptions.
        ~WebGpuDevice() override;

        /// Returns the Dawn device this object wraps, for the command encoders that record into it.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUDevice wgpu_device() const noexcept { return device_; }

        /// Returns the Dawn queue every submission goes to.
        ///
        /// WebGPU exposes exactly one queue. The RHI's queue classes (Graphics/Compute/Transfer)
        /// therefore all resolve to this one, which is why `queue_infos()` reports a single entry.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUQueue wgpu_queue() const noexcept { return queue_; }

        [[nodiscard]] rhi::BackendType backend_type() const noexcept override;
        [[nodiscard]] const rhi::AdapterInfo &adapter_info() const noexcept override;
        [[nodiscard]] const rhi::DeviceLimits &limits() const noexcept override;
        [[nodiscard]] const rhi::FeatureNegotiationReport &feature_negotiation_report() const noexcept override;
        [[nodiscard]] const rhi::FeatureSet &enabled_features() const noexcept override;
        [[nodiscard]] const rhi::FeatureProperties &feature_properties() const noexcept override;
        [[nodiscard]] span<const rhi::QueueInfo> queue_infos() const noexcept override;
        [[nodiscard]] span<const rhi::ExtensionId> enabled_extensions() const noexcept override;
        [[nodiscard]] rhi::RhiDeviceExtension *extension_interface(rhi::ExtensionId extension) noexcept override;
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
        [[nodiscard]] rhi::RhiExpected<rhi::RenderPipelineHandle> create_render_pipeline(const rhi::RenderPipelineDesc &desc) override;
        void destroy_render_pipeline(rhi::RenderPipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::ComputePipelineHandle> create_compute_pipeline(const rhi::ComputePipelineDesc &desc) override;
        void destroy_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::RayTracingPipelineHandle> create_ray_tracing_pipeline( const rhi::RayTracingPipelineDesc &desc) override;
        void destroy_ray_tracing_pipeline(rhi::RayTracingPipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult write_ray_tracing_shader_group_handles( rhi::RayTracingPipelineHandle pipeline, u32 first_group, u32 group_count, span<std::byte> dst) override;
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> acceleration_structure_build_sizes( const rhi::AccelerationStructureBuildDesc &desc) const override;
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureHandle> create_acceleration_structure( const rhi::AccelerationStructureDesc &desc) override;
        void destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::OpacityMicromapBuildSizes> opacity_micromap_build_sizes( const rhi::OpacityMicromapDesc &desc) const override;
        [[nodiscard]] rhi::RhiExpected<rhi::OpacityMicromapHandle> create_opacity_micromap( const rhi::OpacityMicromapDesc &desc, u64 size) override;
        void destroy_opacity_micromap(rhi::OpacityMicromapHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<u64> buffer_device_address(rhi::BufferHandle buffer) const override;
        [[nodiscard]] rhi::RhiExpected<u64> acceleration_structure_device_address( rhi::AccelerationStructureHandle handle) const override;
        [[nodiscard]] rhi::RhiResult write_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) override;
        [[nodiscard]] rhi::RhiExpected<span<std::byte>> map_buffer(rhi::BufferHandle buffer) override;
        void unmap_buffer(rhi::BufferHandle buffer) noexcept override;
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> create_command_encoder( const rhi::CommandEncoderDesc &desc) override;
        void destroy_command_buffer(rhi::CommandBufferHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> create_render_bundle_encoder( const rhi::RenderBundleDesc &desc) override;
        void destroy_render_bundle(rhi::RenderBundleHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult submit(const rhi::SubmitDesc &desc) override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHandle> create_surface(const rhi::SurfaceDesc &desc) override;
        void destroy_surface(rhi::SurfaceHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SwapchainHandle> create_swapchain(const rhi::SwapchainDesc &desc) override;
        void destroy_swapchain(rhi::SwapchainHandle handle) noexcept override;
        [[nodiscard]] rhi::PresentationResolution presentation_resolution(rhi::SwapchainHandle handle) const noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> query_hdr_capabilities( rhi::SurfaceHandle handle) const override;
        [[nodiscard]] rhi::RhiResult update_hdr_content_light_level( rhi::SwapchainHandle handle, const rhi::HdrContentLightLevelUpdate &update) override;
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
        [[nodiscard]] rhi::RhiResult get_query_set_results(rhi::QuerySetHandle query_set, u32 first, u32 count, span<std::byte> dst, u64 stride, rhi::QueryResultFlags flags = rhi::QueryResultFlags::Result64Bit) override;
        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) noexcept override;
        void wait_idle() noexcept override;

        /// Resolves a buffer handle to the Dawn buffer behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUBuffer lookup_buffer(rhi::BufferHandle handle) noexcept;

        /// Resolves a texture view handle to the Dawn texture view behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUTextureView lookup_texture_view(rhi::TextureViewHandle handle) noexcept;

        /// Resolves a texture handle to the Dawn texture behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUTexture lookup_texture(rhi::TextureHandle handle) noexcept;

        /// The parts of a texture's description a buffer/texture copy has to know to work out the
        /// row pitch WebGPU asks for in bytes.
        struct TextureLayout {
            rhi::Format format = rhi::Format::Undefined;
            rhi::Extent3D extent{};
            u32 mip_levels = 1;
        };

        /// Resolves a texture handle to the format and extent it was created with.
        ///
        /// @param handle Handle identifying the target object or resource.
        /// @param out Receives the layout when the handle is known; left untouched otherwise.
        ///
        /// @return Returns `true` when the handle named a live texture.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool lookup_texture_layout(rhi::TextureHandle handle, TextureLayout &out) noexcept;

        /// Resolves a bind group handle to the Dawn bind group behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUBindGroup lookup_bind_group(rhi::BindGroupHandle handle) noexcept;

        /// Resolves a render pipeline handle to the Dawn pipeline behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPURenderPipeline lookup_render_pipeline(rhi::RenderPipelineHandle handle) noexcept;

        /// Resolves a compute pipeline handle to the Dawn pipeline behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUComputePipeline lookup_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept;

        /// Resolves a query set handle to the Dawn query set behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUQuerySet lookup_query_set(rhi::QuerySetHandle handle) noexcept;

        /// Resolves a render bundle handle to the Dawn bundle behind it.
        ///
        /// @param handle Handle identifying the target object or resource.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPURenderBundle lookup_render_bundle(rhi::RenderBundleHandle handle) noexcept;

        /// Takes ownership of a finished Dawn command buffer and returns the handle naming it.
        ///
        /// @param command_buffer `command_buffer` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] rhi::CommandBufferHandle store_command_buffer(WGPUCommandBuffer command_buffer);

        /// Takes ownership of a finished Dawn render bundle and returns the handle naming it.
        ///
        /// @param bundle `bundle` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] rhi::RenderBundleHandle store_render_bundle(WGPURenderBundle bundle);

        /// Blocks until `future` completes, pumping the Dawn instance while it waits.
        ///
        /// Dawn's async entry points return a `WGPUFuture` rather than calling back inline. The RHI
        /// this backend implements is synchronous throughout, so every such call is resolved here
        /// before returning.
        ///
        /// @param future `future` value used by the operation.
        ///
        /// @return Returns `true` when the future completed; `false` on timeout.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool wait_for(WGPUFuture future) noexcept;

        // ─── Push-constant emulation ─────────────────────────────────────────────
        //
        // WebGPU has no push constants. The portable stand-in, and what the shader library's
        // `SFT_EMULATE_PUSH_CONSTANTS` path is written against, is an ordinary uniform buffer bound
        // at a reserved set with a dynamic offset: each `set_push_constants` copies the block into a
        // fresh slice of a device-wide ring and rebinds group 3 at that slice's offset.
        //
        // The reserved group index is the last one WebGPU guarantees (maxBindGroups is 4), which is
        // why the shaders use `[[vk::binding(0, 3)]]` and not some lower set that real bindings
        // would collide with.

        /// The bind group index reserved for the emulated push-constant block.
        static constexpr u32 push_constant_group_index = 3;

        /// Largest push-constant block this backend accepts, in bytes.
        ///
        /// Vulkan guarantees 128 and the largest block this engine uses is exactly that (two
        /// mat4s, in the GPU culling pass); the extra headroom costs nothing because a slice is
        /// rounded up to the uniform offset alignment anyway.
        static constexpr u32 push_constant_block_size = 256;

        /// What `set_push_constants` needs to bind the reserved group for one draw.
        struct PushConstantBinding {
            WGPUBindGroup group = nullptr;
            u32 dynamic_offset = 0;
        };

        /// Copies `data` into a fresh ring slice and returns what to bind for it.
        ///
        /// The bind group is returned alongside the offset rather than fetched separately because
        /// growing the ring replaces it: handing both back from under one lock is what stops a
        /// caller from pairing an offset with the group it no longer belongs to.
        ///
        /// @param data The whole push-constant block, as the encoder's shadow copy holds it.
        ///
        /// @return Returns the group and dynamic offset to bind, or `std::nullopt` when no slice
        ///         could be obtained.
        /// @note This function does not throw exceptions.
        [[nodiscard]] std::optional<PushConstantBinding> allocate_push_constant_slice(
            span<const std::byte> data) noexcept;

        /// Returns the bind group layout the reserved group is declared with, creating it on first
        /// use. `create_pipeline_layout` appends this so a pipeline's layout matches what
        /// `set_push_constants` binds.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUBindGroupLayout push_constant_bind_group_layout() noexcept;

        /// Returns an empty bind group layout, for padding a pipeline layout's unused lower groups
        /// up to the reserved index. Created once and shared.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUBindGroupLayout empty_bind_group_layout() noexcept;

        /// Records that everything allocated from the ring so far belongs to the submission just
        /// made, so those bytes can be reclaimed once it completes.
        ///
        /// @note This function does not throw exceptions.
        void mark_push_constants_submitted() noexcept;

        /// Advances the queue by one empty submission, then drains and ticks so Dawn's fenced
        /// deleter actually runs the deletions it has queued against the pending serial.
        ///
        /// @note This function does not throw exceptions.
        void flush_deferred_deletions() noexcept;

        /// Releases everything the push-constant emulation owns. For the destructor, which has
        /// already drained the queue.
        ///
        /// @note This function does not throw exceptions.
        void destroy_push_constant_state() noexcept;

      private:
        /// Everything about the push-constant ring that changes as frames are recorded.
        ///
        /// Behind one lock because the renderer records from several threads and submits from
        /// another, and every one of them allocates from this.
        struct PushConstantState {
            WGPUBuffer ring = nullptr;
            WGPUBindGroup bind_group = nullptr;
            u64 capacity = 0;
            // Next free byte. Returns to zero only when no submission is outstanding.
            u64 cursor = 0;
            // Slice stride: the block size rounded up to the device's dynamic-offset alignment.
            u32 stride = 0;
            // One entry per outstanding submission that read from the ring, in submission order.
            vector<WGPUFuture> claims;
            // Rings and groups replaced by growth. Still named by commands that have not finished,
            // so they are released only once `claims` drains.
            vector<WGPUBuffer> retired_rings;
            vector<WGPUBindGroup> retired_groups;
        };

        /// Creates the reserved group's layout on first use. The ring lock must already be held.
        ///
        /// @return Returns a pointer to the requested object/resource, or `nullptr` when it is unavailable.
        /// @note This function does not throw exceptions.
        [[nodiscard]] WGPUBindGroupLayout push_constant_bind_group_layout_locked() noexcept;

        /// Replaces the ring with one of at least `required` bytes, retiring the old buffer and
        /// bind group until the submissions still reading them complete.
        ///
        /// @param state The locked ring state.
        /// @param layout Bind group layout the new group is created against.
        /// @param required Smallest capacity the new ring must have.
        ///
        /// @return Returns `true` when a new ring and its bind group were created.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool grow_push_constant_ring(PushConstantState &state, WGPUBindGroupLayout layout,
                                                   u64 required) noexcept;

        /// Drops the claims of submissions that have completed, and with them anything retired that
        /// no outstanding submission can still be reading.
        ///
        /// @param state The locked ring state.
        ///
        /// @note This function does not throw exceptions.
        void reclaim_push_constant_slices(PushConstantState &state) noexcept;

        /// One entry per live texture, keeping the description alive so copies can recover the
        /// texel size and extent Dawn does not report back.
        struct TextureEntry {
            WGPUTexture texture = nullptr;
            rhi::Format format = rhi::Format::Undefined;
            rhi::Extent3D extent{};
            u32 mip_levels = 1;
            // True for a texture that came out of a swapchain acquire: those are owned by the
            // surface and must not be released here.
            bool owned_by_surface = false;
        };

        /// One entry per live buffer, keeping the size and mapping state the RHI exposes.
        ///
        /// WebGPU makes its two host-mapping usages mutually exclusive with almost everything else:
        /// a buffer carrying MapWrite may carry only CopySrc alongside it, and one carrying MapRead
        /// may carry only CopyDst. The RHI has no such rule -- a HostUpload buffer there is
        /// routinely also a uniform or storage buffer -- so `map_buffer` cannot be a thin wrapper
        /// over `wgpuBufferMapAsync` and the two extra members below are what stand in for it.
        struct BufferEntry {
            WGPUBuffer buffer = nullptr;
            u64 size = 0;
            rhi::MemoryLocation memory = rhi::MemoryLocation::DeviceLocal;
            // Non-null while the buffer is mapped through Dawn itself, which happens only for a
            // readback buffer (this one, or `readback` below when this one cannot carry MapRead).
            void *mapped = nullptr;
            // Host-side contents of a HostUpload buffer that could not be given MapWrite. Allocated
            // on the first map and kept afterwards so a caller that rewrites only part of the
            // buffer still sees what it wrote last time, then flushed to the GPU on unmap.
            vector<std::byte> shadow{};
            // True between map_buffer and unmap_buffer for the `shadow` path above.
            bool shadow_mapped = false;
            // Companion MapRead|CopyDst buffer for a HostReadback buffer whose other usages barred
            // MapRead. Created on the first map; contents are copied into it on each map.
            WGPUBuffer readback = nullptr;
        };

        /// One entry per live surface, holding the configuration a swapchain applied to it.
        struct SurfaceEntry {
            WGPUSurface surface = nullptr;
            rhi::Format format = rhi::Format::Undefined;
            u32 width = 0;
            u32 height = 0;
            bool configured = false;
            // Which swapchain handle the current configuration belongs to.
            //
            // A WebGPU surface holds exactly one configuration, but the RHI lets several swapchain
            // handles name the same surface -- a caller recreating a swapchain on resize routinely
            // creates the replacement before destroying the original. Without this, destroying the
            // original tears down the configuration the replacement is depending on.
            rhi::SwapchainHandle configured_by{};
        };

        WGPUInstance instance_ = nullptr;
        WGPUDevice device_ = nullptr;
        WGPUQueue queue_ = nullptr;

        rhi::AdapterInfo adapter_info_{};
        rhi::DeviceLimits limits_{};
        rhi::FeatureSet enabled_features_{};
        rhi::FeatureProperties feature_properties_{};
        rhi::FeatureNegotiationReport negotiation_report_{};
        vector<rhi::QueueInfo> queue_infos_{};

        WebGpuResourcePool<rhi::BufferHandle, BufferEntry> buffers_;
        WebGpuResourcePool<rhi::TextureHandle, TextureEntry> textures_;
        WebGpuResourcePool<rhi::TextureViewHandle, WGPUTextureView> texture_views_;
        WebGpuResourcePool<rhi::SamplerHandle, WGPUSampler> samplers_;
        WebGpuResourcePool<rhi::ShaderModuleHandle, WGPUShaderModule> shader_modules_;
        WebGpuResourcePool<rhi::BindGroupLayoutHandle, WGPUBindGroupLayout> bind_group_layouts_;
        WebGpuResourcePool<rhi::BindGroupHandle, WGPUBindGroup> bind_groups_;
        WebGpuResourcePool<rhi::PipelineLayoutHandle, WGPUPipelineLayout> pipeline_layouts_;
        WebGpuResourcePool<rhi::RenderPipelineHandle, WGPURenderPipeline> render_pipelines_;
        WebGpuResourcePool<rhi::ComputePipelineHandle, WGPUComputePipeline> compute_pipelines_;
        WebGpuResourcePool<rhi::CommandBufferHandle, WGPUCommandBuffer> command_buffers_;
        WebGpuResourcePool<rhi::RenderBundleHandle, WGPURenderBundle> render_bundles_;
        WebGpuResourcePool<rhi::SurfaceHandle, SurfaceEntry> surfaces_;
        WebGpuResourcePool<rhi::SwapchainHandle, rhi::SurfaceHandle> swapchains_;
        WebGpuResourcePool<rhi::QuerySetHandle, WGPUQuerySet> query_sets_;

        // WebGPU has neither semaphores nor fences: its single queue is in-order and the
        // implementation derives every dependency itself. These pools model the RHI's primitives as
        // the plain counters they reduce to on such a queue (see WebGpuDeviceSync.cpp).
        WebGpuResourcePool<rhi::SemaphoreHandle, u64> semaphore_values_;
        WebGpuResourcePool<rhi::FenceHandle, bool> fences_;


        Async::Mutex<PushConstantState> push_constants_;
        // Created once on first use and never replaced, so these sit outside the lock above; the
        // lock is still taken while creating them so two threads cannot both make one.
        WGPUBindGroupLayout push_constant_bind_group_layout_ = nullptr;
        WGPUBindGroupLayout empty_bind_group_layout_ = nullptr;
    };

} // namespace SFT::Core::WebGpu
