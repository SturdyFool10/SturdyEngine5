module;

#pragma region Imports
#include <type_traits>
#pragma endregion

export module Sturdy.RHI:Resources;

import Sturdy.Foundation;
import :Flags;
import :Types;
import :Handles;

export namespace SFT::RHI {

    // ─── Buffers ─────────────────────────────────────────────────────────────────

    // What a buffer may be bound as. A bitmask — a buffer is frequently several at once (e.g. a
    // TransferDst | Vertex device-local buffer seeded from a staging copy). `enable_flag_ops` below
    // wires up the `|`/`&`/`has_any` operator set (see :Flags).
    enum class BufferUsage : u32 {
        None = 0,
        TransferSrc = 1u << 0, // copy source (e.g. a staging buffer)
        TransferDst = 1u << 1, // copy destination (e.g. a device-local buffer being seeded)
        Vertex = 1u << 2,
        Index = 1u << 3,
        Uniform = 1u << 4, // read-only, small, frequently-updated constants
        Storage = 1u << 5,                 // read/write shader storage (SSBO / UAV)
        Indirect = 1u << 6,                // source of indirect draw/dispatch arguments
        ShaderBindingTable = 1u << 7,      // ray tracing shader binding table records
        AccelerationStructure = 1u << 8,   // backing storage for an acceleration structure
        AccelerationStructureInput = 1u << 9, // geometry/instance input read during AS builds
    };

    // Where a buffer's memory lives / how the CPU reaches it. The backend picks the concrete heap;
    // this only states intent, so the same descriptor works whether the API is Vulkan+VMA, D3D12
    // heaps, or Metal storage modes.
    enum class MemoryLocation : u32 {
        // Device-local, not CPU-mappable. The fast default for anything the GPU reads every frame;
        // upload to it via a staging buffer + copy.
        DeviceLocal,
        // Host-visible and optimized for the CPU writing sequentially then the GPU reading — the
        // classic staging/upload buffer, or a per-frame uniform buffer written each frame.
        HostUpload,
        // Host-visible and optimized for the GPU writing then the CPU reading back (readbacks).
        HostReadback,
    };

    struct BufferDesc {
        u64 size = 0;
        BufferUsage usage = BufferUsage::None;
        MemoryLocation memory = MemoryLocation::DeviceLocal;
        // Optional debug name surfaced to the API's object-labeling extension (e.g.
        // VK_EXT_debug_utils) — cheap and worth setting; named resources are the difference between
        // a legible capture/validation message and a raw handle.
        const char *label = nullptr;
    };

    // ─── Textures ────────────────────────────────────────────────────────────────

    enum class TextureDimension : u32 {
        Dim1D,
        Dim2D,
        Dim3D,
    };

    enum class TextureUsage : u32 {
        None = 0,
        TransferSrc = 1u << 0,
        TransferDst = 1u << 1,
        Sampled = 1u << 2,          // read through a sampler in a shader
        Storage = 1u << 3,          // read/write image load/store in a shader
        ColorAttachment = 1u << 4,  // rendered into as a color target
        DepthStencilAttachment = 1u << 5,
    };

    struct TextureDesc {
        TextureDimension dimension = TextureDimension::Dim2D;
        Format format = Format::Undefined;
        Extent3D extent{};              // depth_or_layers is depth for Dim3D, array layers otherwise
        u32 mip_levels = 1;
        SampleCount samples = SampleCount::X1;
        TextureUsage usage = TextureUsage::None;
        const char *label = nullptr;
    };

    // Which mip/array slices a view exposes, and as what. A view is how a texture is actually bound
    // — as a shader resource, an attachment, or a storage image — possibly reinterpreting a subrange
    // or (with a compatible `format`) the texel format.
    enum class TextureViewType : u32 {
        View1D,
        View2D,
        View2DArray,
        ViewCube,
        ViewCubeArray,
        View3D,
    };

    // Sentinel meaning "the rest of the mips/layers from the base" — mirrors Vulkan's
    // VK_REMAINING_* so a view can say "everything" without querying the texture's counts.
    inline constexpr u32 all_remaining = ~0u;

    struct TextureViewDesc {
        TextureHandle texture{};
        TextureViewType view_type = TextureViewType::View2D;
        // Undefined => inherit the source texture's format (the common case); set only to
        // reinterpret to a format in the same compatibility class.
        Format format = Format::Undefined;
        u32 base_mip_level = 0;
        u32 mip_level_count = all_remaining;
        u32 base_array_layer = 0;
        u32 array_layer_count = all_remaining;
        const char *label = nullptr;
    };

    // ─── Samplers ────────────────────────────────────────────────────────────────

    enum class Filter : u32 {
        Nearest,
        Linear,
    };

    enum class MipmapMode : u32 {
        Nearest,
        Linear,
    };

    enum class AddressMode : u32 {
        Repeat,
        MirroredRepeat,
        ClampToEdge,
        ClampToBorder,
    };

    enum class BorderColor : u32 {
        TransparentBlack,
        OpaqueBlack,
        OpaqueWhite,
    };

    // Comparison for depth tests, shadow-map compare samplers, and stencil ops. Shared vocabulary,
    // so it lives here rather than being redefined per pipeline-state struct.
    enum class CompareOp : u32 {
        Never,
        Less,
        Equal,
        LessEqual,
        Greater,
        NotEqual,
        GreaterEqual,
        Always,
    };

    struct SamplerDesc {
        Filter min_filter = Filter::Linear;
        Filter mag_filter = Filter::Linear;
        MipmapMode mipmap_mode = MipmapMode::Linear;
        AddressMode address_u = AddressMode::Repeat;
        AddressMode address_v = AddressMode::Repeat;
        AddressMode address_w = AddressMode::Repeat;
        f32 mip_lod_bias = 0.0f;
        f32 min_lod = 0.0f;
        f32 max_lod = 1000.0f; // effectively "no clamp" — matches the common Vulkan default
        // 0 disables anisotropy; >1 requests that max ratio, clamped to the device limit.
        f32 max_anisotropy = 0.0f;
        // Set `compare` to enable a comparison (shadow) sampler; ignored otherwise.
        bool compare_enable = false;
        CompareOp compare = CompareOp::Never;
        BorderColor border_color = BorderColor::TransparentBlack;
        const char *label = nullptr;
    };

    template <>
    struct enable_flag_ops<BufferUsage> : std::true_type {};
    template <>
    struct enable_flag_ops<TextureUsage> : std::true_type {};

} // namespace SFT::RHI
