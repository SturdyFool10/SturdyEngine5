#pragma once

// Sturdy.RHI's Direct3D 12 device. Every `create_*`/`destroy_*` mints or releases a handle in one of
// the resource pools below; method bodies are split by concern across D3D12Device*.cpp, mirroring how
// the Vulkan backend splits VulkanRhiBridge*.cpp.
//
// ─── The binding-model mapping, stated once ──────────────────────────────────────────────────────
//
// This is where D3D12 diverges most from the Vulkan shape the RHI's :Binding vocabulary was drawn
// from, so the whole mapping is written down here rather than rediscovered per file.
//
//  * A `BindGroupLayout` becomes at most two **descriptor tables** (one CBV/SRV/UAV, one SAMPLER —
//    D3D12 forbids mixing sampler and resource descriptors in a single table) plus zero or more
//    **root descriptors**.
//
//  * `BindGroupLayoutEntry::has_dynamic_offset` entries become root descriptors (root CBV/SRV/UAV)
//    rather than table entries. This is not a shortcut: a descriptor in a heap bakes in the buffer's
//    address, so a dynamic offset simply cannot be applied to one at bind time. A root descriptor is
//    a raw GPU virtual address written straight into the root arguments, so the offset is added there
//    — which is also the fastest binding D3D12 offers, making the constraint and the optimum agree.
//
//  * `PushConstantRange`s become **root constants**. All ranges are merged into one root parameter
//    covering `[0, max(offset + size))` 32-bit values, because D3D12 addresses root constants by a
//    single (root parameter, offset-in-constants) pair and cannot express per-stage sub-ranges of one
//    block the way Vulkan's push-constant ranges do. Stage visibility of the merged parameter is the
//    union of the ranges' stages.
//
//  * **Register assignment ABI** (the contract shaders are compiled against): bind group index *i*
//    maps to HLSL register space *i*, and `BindGroupLayoutEntry::shader_register` maps to the register
//    number within its register class — UniformBuffer to `b#`, SampledTexture/ReadOnlyStorageBuffer/
//    AccelerationStructure/InputAttachment to `t#`, StorageBuffer/StorageTexture to `u#`, Sampler to
//    `s#`. `CombinedImageSampler` occupies *both* `t#` and `s#` at the same number, since D3D12 has no
//    combined descriptor. Push constants live at `b0` in register space
//    `PipelineLayoutDesc::bind_group_layouts.size()` — the first space no bind group can occupy, so
//    they never collide with a real set no matter how many sets a layout declares.
//
// ─── Barriers ────────────────────────────────────────────────────────────────────────────────────
//
// Enhanced barriers (`ID3D12GraphicsCommandList7::Barrier`) are the primary path: they are D3D12's
// port of the very synchronization2 model :Barrier is specified against, so the translation preserves
// structure instead of collapsing it. Devices that report `EnhancedBarriersSupported == FALSE` fall
// back to legacy `ResourceBarrier`, which requires tracking each subresource's current state on the
// device (see `TextureRecord::legacy_states`) because a legacy transition must name the state the
// resource is *coming from* — information the RHI's caller-stated `old_layout` supplies but which
// D3D12 also demands be exactly right rather than merely compatible.

#include <D3D12/D3D12Common.hpp>
#include <D3D12/D3D12Descriptors.hpp>
#include <D3D12/D3D12ResourcePool.hpp>

#pragma region Imports
#include <dcomp.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#pragma endregion

#include <Async/src/Mutex.hpp>

namespace SFT::D3D12 {

    using std::span;
    using std::unique_ptr;
    using std::vector;

    class D3D12CommandEncoder;
    class D3D12RenderPassEncoder;
    class D3D12ComputePassEncoder;
    class D3D12RenderBundleEncoder;

    // ─── Binding-model records ───────────────────────────────────────────────────

    // One bind-group-layout slot that lives in a descriptor table, resolved to its position within
    // that table. `table_offset` is in descriptors from the table's base, which is exactly what
    // D3D12_DESCRIPTOR_RANGE1::OffsetInDescriptorsFromTableStart wants.
    struct TableSlot {
        u32 binding = 0;
        u32 shader_register = 0;
        u32 table_offset = 0;
        u32 count = 1;
        rhi::BindingType type = rhi::BindingType::UniformBuffer;
        rhi::ShaderStage visibility = rhi::ShaderStage::None;
        rhi::BindingFlags flags = rhi::BindingFlags::None;
    };

    // One slot promoted to a root descriptor because it carries a dynamic offset (see this header's
    // binding-model note). The root-parameter index is assigned by the pipeline layout, not here — a
    // layout can be reused by several pipeline layouts at different set indices.
    struct DynamicSlot {
        u32 binding = 0;
        u32 shader_register = 0;
        rhi::BindingType type = rhi::BindingType::UniformBuffer;
        rhi::ShaderStage visibility = rhi::ShaderStage::None;
    };

    struct BindGroupLayoutRecord {
        vector<rhi::BindGroupLayoutEntry> entries;
        vector<TableSlot> resource_slots;
        vector<TableSlot> sampler_slots;
        // In binding order — this order is what BindGroupEntry writes and the render/compute encoders'
        // `dynamic_offsets` span are both indexed against, matching the RHI's documented
        // "one offset per has_dynamic_offset entry, in binding order".
        vector<DynamicSlot> dynamic_slots;
        u32 resource_descriptor_count = 0;
        u32 sampler_descriptor_count = 0;
        // Index into `resource_slots` of a VariableDescriptorCount binding, or ~0u. Its `count` is the
        // maximum; a bind group may allocate fewer (BindGroupDesc::variable_descriptor_count).
        u32 variable_slot_index = ~0u;
    };

    // Where one bind group index's root parameters ended up in a pipeline layout's root signature.
    struct SetRootParameters {
        i32 resource_table = -1;
        i32 sampler_table = -1;
        // Parallel to the layout's `dynamic_slots`.
        vector<i32> dynamic_root_parameters;
    };

    struct PipelineLayoutRecord {
        ComPtr<ID3D12RootSignature> root_signature;
        vector<rhi::BindGroupLayoutHandle> set_layouts;
        vector<SetRootParameters> sets;
        i32 push_constant_root_parameter = -1;
        // Size of the merged root-constant block in 32-bit values (see this header's push-constant
        // note). Bounds-checks set_push_constants without a second lookup of the original ranges.
        u32 push_constant_values = 0;
        // FNV-1a over the serialized root-signature blob (D3D12DeviceBinding.cpp, right after
        // D3DX12SerializeVersionedRootSignature succeeds). Stands in for the root signature when
        // D3D12DevicePipelines.cpp derives a PSO's ID3D12PipelineLibrary cache name: the
        // ComPtr<ID3D12RootSignature> pointer itself is a process-local address and would give the
        // same logical pipeline a different name every run, defeating the whole point of a disk cache.
        u64 root_signature_content_hash = 0;
    };

    struct BindGroupRecord {
        rhi::BindGroupLayoutHandle layout{};
        // Authoritative, never-recycled CPU-heap storage; copied into a command list's shader-visible
        // heap at bind time (see D3D12Descriptors.hpp's own header comment for why this indirection
        // is required rather than merely convenient).
        DescriptorRange resources{};
        DescriptorRange samplers{};
        // Parallel to the layout's `dynamic_slots`: the buffer base address a bind-time dynamic offset
        // is added to. Zero for a slot the caller left unbound, which is a caller error the encoder
        // reports rather than silently binding address 0.
        vector<D3D12_GPU_VIRTUAL_ADDRESS> dynamic_addresses;
    };

    // ─── Resource records ────────────────────────────────────────────────────────

    struct BufferRecord {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address = 0;
        u64 size = 0;
        rhi::MemoryLocation memory = rhi::MemoryLocation::DeviceLocal;
        rhi::BufferUsage usage = rhi::BufferUsage::None;
        // Non-null while the caller holds a map_buffer() span. D3D12 refcounts Map/Unmap internally,
        // but the pointer is cached so repeated maps don't re-enter the driver and so unmap_buffer()
        // is a no-op on an unmapped buffer rather than an unbalanced Unmap.
        void *mapped = nullptr;
        // True when this DeviceLocal buffer was placed on D3D12_HEAP_TYPE_GPU_UPLOAD (ReBAR) instead
        // of D3D12_HEAP_TYPE_DEFAULT. `memory` still reads DeviceLocal — the RHI's documented
        // "not host-mappable" contract for that value is unchanged — this only tells write_buffer()
        // it can write directly instead of staging.
        bool gpu_upload_heap = false;
    };

    struct TextureRecord {
        ComPtr<ID3D12Resource> resource;
        rhi::Format format = rhi::Format::Undefined;
        DXGI_FORMAT resource_format = DXGI_FORMAT_UNKNOWN;
        rhi::TextureDimension dimension = rhi::TextureDimension::Dim2D;
        rhi::Extent3D extent{};
        u32 mip_levels = 1;
        u32 array_layers = 1;
        rhi::SampleCount samples = rhi::SampleCount::X1;
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        // True for a texture the backend does not own the memory of — a DXGI back buffer. Its
        // ID3D12Resource is released with the swapchain, and it must never be destroyed through
        // destroy_texture() alone.
        bool is_swapchain_image = false;
        // Per-subresource current state, maintained only on the legacy-barrier fallback path (see this
        // header's barrier note). Empty when enhanced barriers are in use, which is the common case.
        vector<D3D12_RESOURCE_STATES> legacy_states;
    };

    struct TextureViewRecord {
        rhi::TextureHandle texture{};
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        rhi::Format rhi_format = rhi::Format::Undefined;
        u32 base_mip_level = 0;
        u32 mip_level_count = 1;
        u32 base_array_layer = 0;
        u32 array_layer_count = 1;
        // Created up front for whichever view kinds the source texture's usage permits, so binding and
        // attachment paths never allocate. An unused kind stays an invalid (zero-count) range.
        DescriptorRange srv{};
        DescriptorRange uav{};
        DescriptorRange rtv{};
        DescriptorRange dsv{};
    };

    struct SamplerRecord {
        DescriptorRange descriptor{};
    };

    struct ShaderModuleRecord {
        // DXIL bytecode, copied because ShaderModuleDesc::code is documented non-owning.
        vector<std::byte> bytecode;
    };

    struct RenderPipelineRecord {
        ComPtr<ID3D12PipelineState> pipeline;
        rhi::PipelineLayoutHandle layout{};
        // Bound with IASetPrimitiveTopology at draw time: the PSO stores only the topology *type*
        // (point/line/triangle), not list-vs-strip, which is dynamic state in D3D12.
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        // D3D12 puts the byte stride in each bound vertex-buffer view rather than in the PSO. Retain
        // the RHI pipeline's per-slot values so set_vertex_buffer() can form valid views.
        vector<u32> vertex_strides;
        bool is_mesh_pipeline = false;
    };

    struct ComputePipelineRecord {
        ComPtr<ID3D12PipelineState> pipeline;
        rhi::PipelineLayoutHandle layout{};
    };

    struct RayTracingPipelineRecord {
        ComPtr<ID3D12StateObject> state_object;
        ComPtr<ID3D12StateObjectProperties> properties;
        rhi::PipelineLayoutHandle layout{};
        // Export name per shader group, in the order groups were declared — the order
        // write_ray_tracing_shader_group_handles() indexes with `first_group`.
        vector<std::wstring> group_exports;
    };

    struct AccelerationStructureRecord {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_address = 0;
        u64 size = 0;
        rhi::AccelerationStructureType type = rhi::AccelerationStructureType::BottomLevel;
    };

    // A recorded, closed command list plus the allocator and shader-visible heaps it recorded
    // against. All four are recycled together because none of them may be reused until the
    // submission that consumed the list has completed — which is exactly the condition the RHI puts
    // on destroy_command_buffer().
    struct CommandBufferRecord {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ShaderVisibleDescriptorHeap resource_heap;
        ShaderVisibleDescriptorHeap sampler_heap;
        // Heaps swapped out mid-recording after exhaustion. Retained (not freed) until this record is
        // recycled, because the submitted command list still references descriptors inside them.
        vector<ShaderVisibleDescriptorHeap> retired_heaps;
        // Upload allocations referenced by CopyBufferRegion commands recorded through update/fill.
        // They remain alive until this command record is retired and recycled.
        vector<ComPtr<ID3D12Resource>> transient_uploads;
        // CPU descriptors used by commands such as ClearUnorderedAccessViewUint must outlive GPU
        // execution just like the command list's shader-visible descriptor copies.
        vector<DescriptorRange> transient_resource_descriptors;
        vector<DescriptorRange> transient_rtv_descriptors;
        vector<DescriptorRange> transient_dsv_descriptors;
        rhi::QueueLane queue{};
        D3D12_COMMAND_LIST_TYPE list_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    };

    struct RenderBundleRecord {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        // A D3D12 bundle inherits the *caller's* descriptor heaps and root signature but not its
        // pipeline state; the bind groups it referenced were copied into the parent list's heap at
        // record time, so the bundle keeps its own staging copies alive by handle to guarantee they
        // outlive it.
        vector<rhi::BindGroupHandle> referenced_bind_groups;
    };

    struct SemaphoreRecord {
        // A D3D12 fence *is* a timeline semaphore: a monotonically increasing u64 signalled by a queue
        // or the host and waitable by either. No emulation is needed in this direction at all.
        ComPtr<ID3D12Fence> fence;
    };

    // A host fence. D3D12 has no binary fence, so one is modelled as a monotonic fence plus the value
    // that currently means "signaled": `wait_value == 0` is the unsignaled/unarmed state, and
    // reset_fences() returns to it. See D3D12DeviceSync.cpp for the full state machine.
    struct FenceRecord {
        ComPtr<ID3D12Fence> fence;
        HANDLE event = nullptr;
        u64 next_value = 0;
        u64 wait_value = 0;
        // FenceDesc::signaled, i.e. "already signaled without any GPU work having run". Cleared by
        // reset_fences() and by arming the fence on a submission.
        bool signaled_without_gpu = false;
    };

    struct QuerySetRecord {
        ComPtr<ID3D12QueryHeap> heap;
        rhi::QueryType type = rhi::QueryType::Timestamp;
        u32 count = 0;
        rhi::PipelineStatistic statistics = rhi::PipelineStatistic::None;
        // Scratch readback buffer backing the host-side get_query_set_results(): D3D12 can only
        // resolve query results into a GPU buffer, so a host read is a resolve-then-map, and the
        // buffer is kept per query set rather than reallocated on every call.
        ComPtr<ID3D12Resource> readback;
        u64 readback_bytes = 0;
    };

    struct SurfaceRecord {
        HWND hwnd = nullptr;
        rhi::SurfaceDesc desc{};
        // Owns the label `desc.label` points at, so query_hdr_capabilities() never reads through a
        // caller's possibly-freed pointer.
        std::string stored_label;
    };

    struct SwapchainRecord {
        ComPtr<IDXGISwapChain4> swapchain;
        rhi::SurfaceHandle surface{};
        vector<rhi::TextureHandle> textures;
        vector<rhi::TextureViewHandle> views;
        u32 image_count = 0;
        u32 current_image = ~0u;
        u32 sync_interval = 1;
        UINT present_flags = 0;
        rhi::PresentationResolution presentation_resolution{};
        // The color space SetColorSpace1 successfully selected, or sRGB when negotiation fell back.
        // Metadata behavior keys off this effective value rather than the caller's original request.
        rhi::ColorSpace effective_color_space = rhi::ColorSpace::SrgbNonlinear;
        // Non-null exactly when this swapchain composites through DirectComposition rather than
        // presenting to the HWND directly. Unlike the Vulkan backend — which must round-trip through a
        // shared-texture interop device because DXGI back buffers cannot be imported into Vulkan —
        // D3D12 renders into the composition swapchain's back buffers directly, so this path costs
        // nothing beyond the compositor itself.
        ComPtr<IDCompositionDevice> composition_device;
        ComPtr<IDCompositionTarget> composition_target;
        ComPtr<IDCompositionVisual> composition_visual;
        // Waitable frame-latency object, when the swapchain was created with
        // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT. Waited on in acquire_next_texture() so
        // the CPU is throttled by the presentation engine rather than by the frame it queued three
        // frames ago — the closest DXGI equivalent of Vulkan's acquire-blocks-until-an-image-is-free.
        HANDLE frame_latency_waitable = nullptr;
        // The exact metadata last accepted by SetHDRMetaData. Retained so CLL/FALL updates preserve
        // mastering primaries, white point, and luminance while changing only the two content fields.
        bool has_hdr_metadata = false;
        DXGI_HDR_METADATA_HDR10 stored_hdr_metadata{};

        [[nodiscard]] bool is_composition_present() const noexcept { return composition_target != nullptr; }
    };

    // ─── Device ──────────────────────────────────────────────────────────────────

    struct D3D12DeviceCreateInfo {
        ComPtr<IDXGIFactory6> factory;
        ComPtr<IDXGIAdapter4> adapter;
        ComPtr<ID3D12Device> device;
        rhi::AdapterInfo adapter_info{};
        rhi::DeviceLimits limits{};
        rhi::FeatureNegotiationReport feature_report{};
        rhi::FeatureProperties feature_properties{};
        vector<rhi::QueueInfo> queue_infos;
        vector<rhi::ExtensionId> enabled_extensions;
        bool enhanced_barriers = false;
        bool debug_layer_enabled = false;
        bool allow_tearing = false;
        // D3D12_SHADER_CACHE_SUPPORT_LIBRARY, probed via D3D12_FEATURE_SHADER_CACHE
        // (D3D12Adapter.cpp::probe_capabilities). Gates whether initialize() attempts to stand up the
        // ID3D12PipelineLibrary-backed PSO disk cache at all.
        bool pipeline_library_supported = false;
        // D3D12_HEAP_TYPE_GPU_UPLOAD (ReBAR) capability probed by D3D12Adapter — see
        // DeviceCapabilities::gpu_upload_heap_supported for what it means.
        bool gpu_upload_heap_supported = false;
    };

    class D3D12Device final : public rhi::RhiDevice {
      public:
        explicit D3D12Device(D3D12DeviceCreateInfo &&info);
        ~D3D12Device() override;

        // Brings up the queues, descriptor allocators, and internal upload machinery that a
        // constructor cannot report failure from. Must succeed before the device is handed out.
        [[nodiscard]] rhi::RhiResult initialize();

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

        // ── Resources ──
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

        // ── Pipelines ──
        [[nodiscard]] rhi::RhiExpected<rhi::RenderPipelineHandle> create_render_pipeline(const rhi::RenderPipelineDesc &desc) override;
        void destroy_render_pipeline(rhi::RenderPipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::ComputePipelineHandle> create_compute_pipeline(const rhi::ComputePipelineDesc &desc) override;
        void destroy_compute_pipeline(rhi::ComputePipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::RayTracingPipelineHandle> create_ray_tracing_pipeline(
            const rhi::RayTracingPipelineDesc &desc) override;
        void destroy_ray_tracing_pipeline(rhi::RayTracingPipelineHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult write_ray_tracing_shader_group_handles(
            rhi::RayTracingPipelineHandle pipeline,
            u32 first_group,
            u32 group_count,
            span<std::byte> dst) override;

        // ── Acceleration structures / device addresses ──
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureBuildSizes> acceleration_structure_build_sizes(
            const rhi::AccelerationStructureBuildDesc &desc) const override;
        [[nodiscard]] rhi::RhiExpected<rhi::AccelerationStructureHandle> create_acceleration_structure(
            const rhi::AccelerationStructureDesc &desc) override;
        void destroy_acceleration_structure(rhi::AccelerationStructureHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<u64> buffer_device_address(rhi::BufferHandle buffer) const override;
        [[nodiscard]] rhi::RhiExpected<u64> acceleration_structure_device_address(
            rhi::AccelerationStructureHandle handle) const override;

        // ── Data upload ──
        [[nodiscard]] rhi::RhiResult write_buffer(rhi::BufferHandle buffer, u64 offset, span<const std::byte> data) override;
        [[nodiscard]] rhi::RhiExpected<span<std::byte>> map_buffer(rhi::BufferHandle buffer) override;
        void unmap_buffer(rhi::BufferHandle buffer) noexcept override;

        // ── Command recording / submission ──
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::CommandEncoder>> create_command_encoder(
            const rhi::CommandEncoderDesc &desc) override;
        void destroy_command_buffer(rhi::CommandBufferHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<unique_ptr<rhi::RenderBundleEncoder>> create_render_bundle_encoder(
            const rhi::RenderBundleDesc &desc) override;
        void destroy_render_bundle(rhi::RenderBundleHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult submit(const rhi::SubmitDesc &desc) override;

        // ── Presentation ──
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHandle> create_surface(const rhi::SurfaceDesc &desc) override;
        void destroy_surface(rhi::SurfaceHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SwapchainHandle> create_swapchain(const rhi::SwapchainDesc &desc) override;
        void destroy_swapchain(rhi::SwapchainHandle handle) noexcept override;
        [[nodiscard]] rhi::PresentationResolution presentation_resolution(rhi::SwapchainHandle handle) const noexcept override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceHdrCapabilityQuery> query_hdr_capabilities(
            rhi::SurfaceHandle handle) const override;
        [[nodiscard]] rhi::RhiResult update_hdr_content_light_level(
            rhi::SwapchainHandle handle,
            const rhi::HdrContentLightLevelUpdate &update) override;
        [[nodiscard]] rhi::RhiExpected<rhi::SurfaceTexture> acquire_next_texture(rhi::SwapchainHandle swapchain,
                                                                                 u32 frame_slot_index) override;
        [[nodiscard]] rhi::RhiExpected<rhi::PresentOutcome> present(const rhi::PresentDesc &desc,
                                                                    f64 *queue_lock_wait_ms = nullptr) override;

        // ── Synchronization ──
        [[nodiscard]] rhi::RhiExpected<rhi::SemaphoreHandle> create_semaphore(const rhi::SemaphoreDesc &desc) override;
        void destroy_semaphore(rhi::SemaphoreHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<u64> semaphore_value(rhi::SemaphoreHandle handle) const override;
        [[nodiscard]] rhi::RhiResult wait_semaphore(rhi::SemaphoreHandle handle, u64 value, u64 timeout_ns = rhi::wait_forever) override;
        [[nodiscard]] rhi::RhiResult signal_semaphore(rhi::SemaphoreHandle handle, u64 value) override;
        [[nodiscard]] rhi::RhiExpected<rhi::FenceHandle> create_fence(const rhi::FenceDesc &desc) override;
        void destroy_fence(rhi::FenceHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiExpected<bool> wait_fences(span<const rhi::FenceHandle> fences, bool wait_all = true, u64 timeout_ns = rhi::wait_forever) override;
        [[nodiscard]] rhi::RhiResult reset_fences(span<const rhi::FenceHandle> fences) override;

        // ── Queries ──
        [[nodiscard]] rhi::RhiExpected<rhi::QuerySetHandle> create_query_set(const rhi::QuerySetDesc &desc) override;
        void destroy_query_set(rhi::QuerySetHandle handle) noexcept override;
        [[nodiscard]] rhi::RhiResult get_query_set_results(rhi::QuerySetHandle query_set, u32 first, u32 count, span<std::byte> dst, u64 stride, rhi::QueryResultFlags flags = rhi::QueryResultFlags::Result64Bit) override;
        void reset_query_set(rhi::QuerySetHandle query_set, u32 first, u32 count) noexcept override;

        void wait_idle() noexcept override;

        // Buffer lookup for acceleration-structure build translation, which needs GPU addresses out of
        // buffer handles from both a const device (build-size queries) and the command encoder. Public
        // because build_acceleration_structure_inputs() is a free function shared by the two, rather
        // than a member of either.
        [[nodiscard]] const BufferRecord *find_buffer_for_build(rhi::BufferHandle handle) const noexcept {
            return buffers_.find(handle);
        }

      private:
        friend class D3D12CommandEncoder;
        friend class D3D12RenderPassEncoder;
        friend class D3D12ComputePassEncoder;
        friend class D3D12RenderBundleEncoder;

        // ── Internal helpers (D3D12DeviceCore.cpp unless noted) ──
        [[nodiscard]] ID3D12CommandQueue *queue_for_lane(rhi::QueueLane lane) const noexcept;
        [[nodiscard]] rhi::RhiResult validate_queue_lane(rhi::QueueLane lane, const char *operation) const;

        // Blocking staged upload into a DeviceLocal buffer, backing write_buffer()'s documented
        // behavior for non-host-visible memory. Uses the copy queue when one exists so it never
        // serializes behind graphics work. (D3D12DeviceResources.cpp)
        [[nodiscard]] rhi::RhiResult upload_via_staging(ID3D12Resource *destination, u64 offset, span<const std::byte> data);

        // Direct CPU write into a buffer create_buffer() placed on D3D12_HEAP_TYPE_GPU_UPLOAD (see
        // BufferRecord::gpu_upload_heap): the memory is GPU-local and CPU-mappable at once, so the
        // staging buffer + copy queue upload_via_staging() needs is skipped entirely. (D3D12DeviceResources.cpp)
        [[nodiscard]] rhi::RhiResult write_via_gpu_upload_heap(BufferRecord &record, u64 offset, span<const std::byte> data);

        // Records `record` onto its queue and blocks until it completes. The one-shot path shared by
        // upload_via_staging(), get_query_set_results(), and swapchain image-state initialization.
        [[nodiscard]] rhi::RhiResult execute_and_wait(ID3D12GraphicsCommandList *list, rhi::QueueClass queue);

        // Checks a (allocator, list, heaps) quadruple out of `command_buffer_free_list_` for
        // `list_type`, or nullopt when that type's free list is empty and a fresh one must be created.
        [[nodiscard]] std::optional<CommandBufferRecord> checkout_command_buffer(D3D12_COMMAND_LIST_TYPE list_type) noexcept;
        void return_command_buffer(CommandBufferRecord &&record) noexcept;

        // Builds the (allocator, list, heaps) set an encoder records into. (D3D12CommandEncoder.cpp)
        [[nodiscard]] rhi::RhiExpected<CommandBufferRecord> acquire_command_buffer(rhi::QueueLane lane);

        [[nodiscard]] rhi::RhiExpected<ShaderVisibleDescriptorHeap> create_shader_visible_heap(
            D3D12_DESCRIPTOR_HEAP_TYPE type,
            u32 capacity);

        // Releases every descriptor range a view/bind group owns. Kept out of the pools' locks — see
        // D3D12ResourcePool::extract()'s own doc comment for the lock-ordering reason.
        void release_view_descriptors(TextureViewRecord &record) noexcept;
        void release_bind_group_descriptors(BindGroupRecord &record) noexcept;

        // Populates a swapchain record's per-back-buffer texture/view handles. (D3D12DeviceSwapchain.cpp)
        [[nodiscard]] rhi::RhiResult create_swapchain_textures(SwapchainRecord &record, rhi::Format format, u32 width, u32 height, rhi::TextureUsage usage);
        void destroy_swapchain_textures(SwapchainRecord &record) noexcept;

        // ── Device objects ──
        ComPtr<IDXGIFactory6> factory_;
        ComPtr<IDXGIAdapter4> adapter_;
        ComPtr<ID3D12Device> device_;
        // Optional newer interfaces, null when the runtime/driver predates them. Every use site checks
        // rather than assuming — that is the whole reason they are separate members instead of a
        // single QueryInterface at the point of use.
        ComPtr<ID3D12Device5> device5_;   // DXR: CreateStateObject, GetRaytracingAccelerationStructurePrebuildInfo
        ComPtr<ID3D12Device8> device8_;   // CreateCommittedResource2 / enhanced-barrier-era creation
        ComPtr<ID3D12Device10> device10_; // CreateCommittedResource3 (initial *layout* rather than state)

        // The PSO disk cache (see D3D12DevicePipelines.cpp's cache-name derivation). Populated by
        // initialize() from `.cache/d3d12_pipeline_library.bin` when the runtime reports
        // D3D12_SHADER_CACHE_SUPPORT_LIBRARY, and serialized back to that file in the destructor. Null
        // whenever the feature isn't supported or even an empty library failed to create — every
        // create_render_pipeline()/create_compute_pipeline() call site checks before using it, exactly
        // like device5_/device8_/device10_ above.
        Async::Mutex<ComPtr<ID3D12PipelineLibrary1>> pipeline_library_;

        ComPtr<ID3D12CommandQueue> graphics_queue_;
        // Null when the device exposes no dedicated engine for that class; queue_for_lane() then
        // aliases the class onto the graphics queue, which is what QueueInfo already reported.
        ComPtr<ID3D12CommandQueue> compute_queue_;
        ComPtr<ID3D12CommandQueue> copy_queue_;

        // One monotonic fence per queue plus a shared event, backing execute_and_wait() and
        // wait_idle(). Guarded because both may be reached from several threads.
        struct BlockingFence {
            ComPtr<ID3D12Fence> fence;
            HANDLE event = nullptr;
            u64 next_value = 0;
        };
        Async::Mutex<BlockingFence> graphics_blocking_fence_;
        Async::Mutex<BlockingFence> compute_blocking_fence_;
        Async::Mutex<BlockingFence> copy_blocking_fence_;

        // Serializes Present() plus the swapchain-state mutation around it, and is what
        // present()'s `queue_lock_wait_ms` measures contention on.
        Async::Mutex<int> present_lock_;

        // ── Descriptor storage ──
        CpuDescriptorAllocator cpu_resource_descriptors_; // CBV/SRV/UAV
        CpuDescriptorAllocator cpu_sampler_descriptors_;
        CpuDescriptorAllocator cpu_rtv_descriptors_;
        CpuDescriptorAllocator cpu_dsv_descriptors_;

        // ── Reported state ──
        rhi::AdapterInfo adapter_info_{};
        rhi::DeviceLimits limits_{};
        rhi::FeatureNegotiationReport feature_report_{};
        rhi::FeatureSet enabled_features_{};
        rhi::FeatureProperties feature_properties_{};
        vector<rhi::QueueInfo> queue_infos_;
        vector<rhi::ExtensionId> enabled_extensions_;
        bool enhanced_barriers_ = false;
        bool debug_layer_enabled_ = false;
        bool allow_tearing_ = false;
        // Whether pipeline_library_ is actually usable. Starts as the capability probe's answer, but
        // initialize() also clears it if standing up even an empty ID3D12PipelineLibrary fails, so
        // every other call site can trust this single flag rather than separately null-checking the
        // ComPtr under the mutex.
        bool pipeline_library_supported_ = false;
        // See D3D12DeviceCreateInfo::gpu_upload_heap_supported. Read by create_buffer() to decide
        // whether a DeviceLocal buffer can be placed on D3D12_HEAP_TYPE_GPU_UPLOAD.
        bool gpu_upload_heap_supported_ = false;

        // ── Resource pools ──
        D3D12ResourcePool<rhi::BufferHandle, BufferRecord> buffers_;
        D3D12ResourcePool<rhi::TextureHandle, TextureRecord> textures_;
        D3D12ResourcePool<rhi::TextureViewHandle, TextureViewRecord> texture_views_;
        D3D12ResourcePool<rhi::SamplerHandle, SamplerRecord> samplers_;
        D3D12ResourcePool<rhi::ShaderModuleHandle, ShaderModuleRecord> shader_modules_;
        D3D12ResourcePool<rhi::BindGroupLayoutHandle, BindGroupLayoutRecord> bind_group_layouts_;
        D3D12ResourcePool<rhi::BindGroupHandle, BindGroupRecord> bind_groups_;
        D3D12ResourcePool<rhi::PipelineLayoutHandle, PipelineLayoutRecord> pipeline_layouts_;
        D3D12ResourcePool<rhi::RenderPipelineHandle, RenderPipelineRecord> render_pipelines_;
        D3D12ResourcePool<rhi::ComputePipelineHandle, ComputePipelineRecord> compute_pipelines_;
        D3D12ResourcePool<rhi::RayTracingPipelineHandle, RayTracingPipelineRecord> ray_tracing_pipelines_;
        D3D12ResourcePool<rhi::AccelerationStructureHandle, AccelerationStructureRecord> acceleration_structures_;
        D3D12ResourcePool<rhi::CommandBufferHandle, CommandBufferRecord> command_buffers_;
        D3D12ResourcePool<rhi::RenderBundleHandle, RenderBundleRecord> render_bundles_;
        D3D12ResourcePool<rhi::SemaphoreHandle, SemaphoreRecord> semaphores_;
        D3D12ResourcePool<rhi::FenceHandle, FenceRecord> fences_;
        D3D12ResourcePool<rhi::QuerySetHandle, QuerySetRecord> query_sets_;
        D3D12ResourcePool<rhi::SurfaceHandle, SurfaceRecord> surfaces_;
        D3D12ResourcePool<rhi::SwapchainHandle, SwapchainRecord> swapchains_;

        // Per-command-list-type free list of reusable (allocator, list, heaps) records, keyed by
        // D3D12_COMMAND_LIST_TYPE because a command allocator is only ever compatible with the type it
        // was created for. Grows lazily to the real peak concurrent-encoder count, then stays there.
        Async::Mutex<std::unordered_map<u32, vector<CommandBufferRecord>>> command_buffer_free_list_;

        // Command signatures for the indirect draw/dispatch paths, created on demand and cached: D3D12
        // requires an ID3D12CommandSignature object describing the argument layout for every
        // ExecuteIndirect, where Vulkan takes the layout implicitly from which vkCmdDraw*Indirect was
        // called. Keyed by argument type. (D3D12CommandEncoder.cpp)
        enum class IndirectKind : u32 { Draw,
                                        DrawIndexed,
                                        Dispatch,
                                        DispatchMesh,
                                        DispatchRays };
        [[nodiscard]] rhi::RhiExpected<ID3D12CommandSignature *> indirect_signature(IndirectKind kind, u32 stride);
        Async::Mutex<std::unordered_map<u64, ComPtr<ID3D12CommandSignature>>> indirect_signatures_;
    };

    // Translates an RHI acceleration-structure build into DXR's single fused inputs structure (D3D12
    // folds Vulkan's geometry array + parallel build-range array into one). Shared by
    // D3D12Device::acceleration_structure_build_sizes() and the command encoder's
    // build_acceleration_structures(), so the translation exists exactly once. `geometry_storage`
    // receives the per-geometry descriptions that `inputs` points into and must outlive it.
    [[nodiscard]] rhi::RhiResult build_acceleration_structure_inputs(
        const D3D12Device &device,
        const rhi::AccelerationStructureBuildDesc &desc,
        vector<D3D12_RAYTRACING_GEOMETRY_DESC> &geometry_storage,
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS &inputs);

} // namespace SFT::D3D12
