#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <type_traits>
#pragma endregion

#include "Flags.hpp"
#include "Handles.hpp"
#include "Shader.hpp"

using std::span;

namespace SFT::RHI {

    // What kind of resource a shader binding slot expects. Kept to the core set every API models the
    // same way; the layout entry below carries the type-specific details.
    enum class BindingType : u32 {
        UniformBuffer,        // read-only constants (UBO / cbuffer)
        StorageBuffer,        // read/write storage (SSBO / UAV buffer)
        ReadOnlyStorageBuffer,
        SampledTexture,       // texture read through a separate sampler
        StorageTexture,          // image load/store
        Sampler,                 // a standalone sampler object
        CombinedImageSampler,    // texture+sampler as one binding (Vulkan-style convenience)
        AccelerationStructure,   // top-level AS for ray queries / ray tracing pipelines
        // A render-pass attachment read at the current fragment's location — the tile-local G-buffer
        // read a deferred lighting pass does inside one dynamic-rendering pass (no barrier, stays in
        // tile memory on TBDR GPUs). Requires Feature::DynamicRenderingLocalRead; `input_attachment_index`
        // on the layout entry selects which color/depth attachment this slot reads.
        InputAttachment,
    };

    // Descriptor-indexing flags for one bind-group layout slot — the difference between a plain array
    // binding (`count` fixed, every element written before use) and a true bindless table. A bitmask
    // (see :Flags); each bit is gated by a device feature the caller must have enabled:
    //   - PartiallyBound          — not every element need be written before the descriptor set is
    //                               used, as long as the shader only accesses written ones
    //                               (Feature::DescriptorBindingPartiallyBound). The core of a bindless
    //                               texture table whose slots fill in over time.
    //   - UpdateAfterBind         — the descriptor may be updated after the set is bound and even while
    //                               command buffers referencing it are pending, so long as the update
    //                               doesn't race the exact element in flight
    //                               (Feature::DescriptorBindingSampledImageUpdateAfterBind et al.).
    //                               Forces the layout/pool onto the update-after-bind path.
    //   - VariableDescriptorCount — `count` is the *maximum*; the actual size is chosen at
    //                               bind-group-allocation time (Feature::DescriptorBindingVariableCount).
    //                               Must be the last binding in its set. Pairs with a runtime-sized
    //                               (`Texture2D t[]`) array in the shader.
    enum class BindingFlags : u32 {
        None = 0,
        PartiallyBound = 1u << 0,
        UpdateAfterBind = 1u << 1,
        VariableDescriptorCount = 1u << 2,
    };

    // One slot in a bind-group layout: its `binding` index, what it holds, which stages see it, and
    // how many array elements it has (`count > 1` is an array binding; a bindless table is a large
    // `count` plus `BindingFlags`). `has_dynamic_offset` marks a uniform/storage buffer whose
    // offset is supplied at bind time (see BindGroupEntry / the render-pass encoder's set_bind_group).
    struct BindGroupLayoutEntry {
        u32 binding = 0;
        BindingType type = BindingType::UniformBuffer;
        ShaderStage visibility = ShaderStage::None;
        u32 count = 1;
        bool has_dynamic_offset = false;
        // Descriptor-indexing flags for this slot (bindless). None for an ordinary binding. When any
        // entry in a layout carries UpdateAfterBind, the whole layout and any pool allocating from it
        // move onto the update-after-bind path; a VariableDescriptorCount entry must be the layout's
        // last binding, and its `count` is the maximum array size.
        BindingFlags flags = BindingFlags::None;
        // Only for BindingType::InputAttachment: which render-pass attachment (its index among the
        // pass's color attachments, or `all_remaining` for the depth/stencil attachment) this slot
        // reads. Ignored for every other binding type.
        u32 input_attachment_index = 0;
    };

    template <>
    struct enable_flag_ops<BindingFlags> : std::true_type {};

    // The shape of one bind group (one descriptor set): the entries a pipeline expects at a given
    // set index. `entries` is non-owning — the backend consumes it during
    // create_bind_group_layout() (see :Device).
    struct BindGroupLayoutDesc {
        span<const BindGroupLayoutEntry> entries;
        const char *label = nullptr;
    };

    // A concrete resource bound to one slot when filling a bind group. Exactly one of the handle
    // members is meaningful, per the layout entry's BindingType; `offset`/`size` scope a buffer
    // binding to a sub-range (size 0 means "to the end").
    struct BindGroupEntry {
        u32 binding = 0;
        BufferHandle buffer{};
        u64 offset = 0;
        u64 size = 0;
        TextureViewHandle texture_view{};
        SamplerHandle sampler{};
        AccelerationStructureHandle acceleration_structure{};
    };

    // A filled-in bind group: concrete resources matching `layout`, ready to bind. `entries` is
    // non-owning (consumed during create_bind_group()).
    struct BindGroupDesc {
        BindGroupLayoutHandle layout{};
        span<const BindGroupEntry> entries;
        const char *label = nullptr;
    };

    // A range of push/root constants — a small block of inline constants written straight into a
    // command buffer (no buffer/bind-group round-trip), scoped to the stages that read it.
    struct PushConstantRange {
        ShaderStage stages = ShaderStage::None;
        u32 offset = 0;
        u32 size = 0;
    };

    // The full interface a pipeline binds against: an ordered list of bind-group layouts (set 0, 1,
    // …) plus any push-constant ranges. Both spans are non-owning (consumed during
    // create_pipeline_layout()).
    struct PipelineLayoutDesc {
        span<const BindGroupLayoutHandle> bind_group_layouts;
        span<const PushConstantRange> push_constant_ranges;
        const char *label = nullptr;
    };

} // namespace SFT::RHI
