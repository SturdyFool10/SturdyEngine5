#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <type_traits>
#pragma endregion

#include "Flags.hpp"
#include "Handles.hpp"
#include "Queues.hpp"

namespace SFT::RHI {

    // ─── Explicit synchronization ────────────────────────────────────────────────
    //
    // The RHI is an *explicit-sync* interface (like Vulkan/D3D12/Metal), not an auto-tracked one
    // (like WebGPU). Auto-tracking is convenient but cannot express everything the underlying APIs
    // can — and a maximal RHI must — so the caller states execution/memory dependencies directly.
    // The model here is Vulkan's synchronization2, which is the most expressive of the three and
    // which D3D12's enhanced barriers and Metal's fences/events both map onto cleanly:
    // (src stage, src access) → (dst stage, dst access), plus an image-layout transition for
    // textures.

    // Pipeline stages a dependency can synchronize against. `u64`-backed to match VkPipelineStage2's
    // width and leave room for future stages without a breaking change. `None` means "no stage" (a
    // pure layout transition or an execution-only sentinel); `AllCommands` is the coarsest hammer.
    enum class PipelineStage : u64 {
        None = 0,
        DrawIndirect = 1ull << 0,
        VertexInput = 1ull << 1,
        VertexShader = 1ull << 2,
        TessControlShader = 1ull << 3,
        TessEvalShader = 1ull << 4,
        GeometryShader = 1ull << 5,
        FragmentShader = 1ull << 6,
        EarlyFragmentTests = 1ull << 7,
        LateFragmentTests = 1ull << 8,
        ColorAttachmentOutput = 1ull << 9,
        ComputeShader = 1ull << 10,
        Transfer = 1ull << 11, // copy/blit/resolve/clear
        Host = 1ull << 12,
        TaskShader = 1ull << 13,
        MeshShader = 1ull << 14,
        RayTracingShader = 1ull << 15,
        AccelerationStructureBuild = 1ull << 16,

        AllGraphics = VertexInput | VertexShader | TessControlShader | TessEvalShader | GeometryShader |
                      FragmentShader | EarlyFragmentTests | LateFragmentTests | ColorAttachmentOutput |
                      TaskShader | MeshShader,
        AllCommands = ~0ull,
    };

    // How memory is accessed on either side of a dependency — the cache-flush/invalidate half of a
    // barrier. `MemoryRead`/`MemoryWrite` are the catch-all "any read/any write" bits for when the
    // precise access set isn't worth spelling out.
    enum class AccessFlags : u64 {
        None = 0,
        IndirectCommandRead = 1ull << 0,
        IndexRead = 1ull << 1,
        VertexAttributeRead = 1ull << 2,
        UniformRead = 1ull << 3,
        ShaderRead = 1ull << 4,
        ShaderWrite = 1ull << 5,
        ColorAttachmentRead = 1ull << 6,
        ColorAttachmentWrite = 1ull << 7,
        DepthStencilAttachmentRead = 1ull << 8,
        DepthStencilAttachmentWrite = 1ull << 9,
        TransferRead = 1ull << 10,
        TransferWrite = 1ull << 11,
        HostRead = 1ull << 12,
        HostWrite = 1ull << 13,
        AccelerationStructureRead = 1ull << 14,
        AccelerationStructureWrite = 1ull << 15,
        MemoryRead = 1ull << 16,
        MemoryWrite = 1ull << 17,
    };

    // The layout a texture's memory is arranged in for a given use. A texture barrier optionally
    // transitions between layouts (old → new); pass the same value for both to synchronize without a
    // transition. `Undefined` as `old_layout` discards the prior contents (cheapest when the next
    // use fully overwrites).
    enum class TextureLayout : u32 {
        Undefined,
        General, // usable by anything (required for storage-image read/write)
        ColorAttachment,
        DepthStencilAttachment,
        DepthStencilReadOnly,
        ShaderReadOnly,
        TransferSrc,
        TransferDst,
        Present,
    };

    // Which mips/layers of a texture a barrier applies to (see `all_remaining` in :Resources for the
    // "everything from the base" sentinels).
    struct TextureSubresourceRange {
        u32 base_mip_level = 0;
        u32 mip_level_count = ~0u;
        u32 base_array_layer = 0;
        u32 array_layer_count = ~0u;
    };

    // A memory dependency not tied to a specific resource — flushes/invalidates for all memory of
    // the given access classes between the two stage sets. The blunt-but-correct option.
    struct GlobalBarrier {
        PipelineStage src_stage = PipelineStage::None;
        AccessFlags src_access = AccessFlags::None;
        PipelineStage dst_stage = PipelineStage::None;
        AccessFlags dst_access = AccessFlags::None;
    };

    // A dependency scoped to one buffer range — e.g. a compute pass finished writing an
    // Indirect/Storage buffer that a subsequent draw reads. `ownership` is normally disabled; set it
    // only when moving a resource between queue classes/lanes that map to distinct native ownership
    // domains (Vulkan queue families). Backends without ownership transfers ignore it.
    struct BufferBarrier {
        BufferHandle buffer{};
        PipelineStage src_stage = PipelineStage::None;
        AccessFlags src_access = AccessFlags::None;
        PipelineStage dst_stage = PipelineStage::None;
        AccessFlags dst_access = AccessFlags::None;
        QueueOwnershipTransfer ownership{};
        u64 offset = 0;
        u64 size = 0; // 0 => to the end of the buffer
    };

    // A dependency scoped to a texture subresource, optionally with a layout transition — the
    // render-to-texture-then-sample and storage-image cases. `ownership` mirrors BufferBarrier and is
    // disabled for normal same-queue transitions.
    struct TextureBarrier {
        TextureHandle texture{};
        PipelineStage src_stage = PipelineStage::None;
        AccessFlags src_access = AccessFlags::None;
        PipelineStage dst_stage = PipelineStage::None;
        AccessFlags dst_access = AccessFlags::None;
        QueueOwnershipTransfer ownership{};
        TextureLayout old_layout = TextureLayout::Undefined;
        TextureLayout new_layout = TextureLayout::Undefined;
        TextureSubresourceRange range{};
    };

    template <>
    struct enable_flag_ops<PipelineStage> : std::true_type {};
    template <>
    struct enable_flag_ops<AccessFlags> : std::true_type {};

} // namespace SFT::RHI
