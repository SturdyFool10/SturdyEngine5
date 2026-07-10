module;

export module Sturdy.RHI:Handles;

import Sturdy.Foundation;

export namespace SFT::RHI {

    // An opaque, strongly-typed reference to one backend-owned resource. The RHI is handle-based
    // rather than object-based on purpose: a description layer shouldn't carry backend object
    // lifetimes or vtables around — `RhiDevice::create_*()` mints a handle, commands reference it,
    // and `RhiDevice::destroy_*()` releases it (see :Device). The `Tag` template parameter makes each
    // resource kind a distinct type, so a `TextureHandle` can never be passed where a `BufferHandle`
    // is expected.
    //
    // `value == 0` is the reserved "null/invalid" handle: a default-constructed handle is invalid,
    // and a backend never hands out 0 for a live resource. The `value` is otherwise entirely the
    // backend's to interpret (a slot index + generation, a pointer bit-cast, an API handle, ...).
    template <class Tag>
    struct Handle {
        u64 value = 0;

        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }

        friend constexpr bool operator==(Handle, Handle) noexcept = default;
    };

    // One tag type per resource kind. They only need to be distinct types, never defined — an
    // incomplete tag is exactly enough to specialize `Handle<>` on.
    using BufferHandle = Handle<struct BufferTag>;
    using TextureHandle = Handle<struct TextureTag>;
    using TextureViewHandle = Handle<struct TextureViewTag>;
    using SamplerHandle = Handle<struct SamplerTag>;
    using ShaderModuleHandle = Handle<struct ShaderModuleTag>;
    using BindGroupLayoutHandle = Handle<struct BindGroupLayoutTag>;
    using BindGroupHandle = Handle<struct BindGroupTag>;
    using PipelineLayoutHandle = Handle<struct PipelineLayoutTag>;
    using RenderPipelineHandle = Handle<struct RenderPipelineTag>;
    using ComputePipelineHandle = Handle<struct ComputePipelineTag>;
    using RayTracingPipelineHandle = Handle<struct RayTracingPipelineTag>;
    using AccelerationStructureHandle = Handle<struct AccelerationStructureTag>;
    using CommandBufferHandle = Handle<struct CommandBufferTag>;
    using RenderBundleHandle = Handle<struct RenderBundleTag>;
    using SurfaceHandle = Handle<struct SurfaceTag>;
    using SwapchainHandle = Handle<struct SwapchainTag>;
    using SemaphoreHandle = Handle<struct SemaphoreTag>;
    using FenceHandle = Handle<struct FenceTag>;
    using QuerySetHandle = Handle<struct QuerySetTag>;

} // namespace SFT::RHI
