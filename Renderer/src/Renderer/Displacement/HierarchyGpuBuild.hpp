#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <optional>
#include <span>
#include <vector>
#pragma endregion

#include <Renderer/Displacement/HeightfieldHierarchy.hpp>

using std::optional;
using std::span;
using std::vector;

// Host side of Shaders/heightfield_hierarchy_build.slang (HierarchyBuild::Compute): where the hierarchy
// lives in the storage buffer the shader writes, the dispatch list for a full or dirty-tile build, and a
// line-for-line CPU model of the shader (so the pass planner is testable without a GPU and a GPU run has
// a byte-exact expectation: HeightfieldHierarchy::pack()).
//
// Replay contract for the Renderer (or a test): for each pass in order, bind the height texture + the
// output buffer, push `constants`, dispatch (group_count_x, group_count_y, 1); insert a compute->compute
// buffer barrier whenever `constants.level` changes from the previous pass; when all passes are done,
// copy each mip of the buffer to the hierarchy texture (mip m = level m + 1) using the layout's offsets and
// row pitches. Build the layout with row_pitch_alignment 256 for D3D12/WebGPU-legal texture copies.
namespace SFT::Renderer::Displacement {

    /// Mirror of the shader's HierarchyBuildConstants (push constants, 17 x u32). Field order matters.
    struct HierarchyBuildConstants {
        u32 src_width = 0;
        u32 src_height = 0;
        u32 wrap = 0;
        /// bit 0: Float32 (else Unorm16); bit 1: two channels (min, max).
        u32 format = 0;
        u32 level = 0;
        u32 level_width = 0;
        u32 level_height = 0;
        u32 child_width = 0;
        u32 child_height = 0;
        u32 rect_x0 = 0;
        u32 rect_y0 = 0;
        u32 rect_x1 = 0;
        u32 rect_y1 = 0;
        u32 out_offset_words = 0;
        u32 out_pitch_words = 0;
        u32 in_offset_words = 0;
        u32 in_pitch_words = 0;
    };
    static_assert(sizeof(HierarchyBuildConstants) == 17 * sizeof(u32));

    inline constexpr u32 kHierarchyFormatFloat32 = 1;
    inline constexpr u32 kHierarchyFormatTwoChannels = 2;

    /// Where each hierarchy mip sits in the build buffer. Every row starts on a word boundary (the shader
    /// writes whole 32-bit words), so a mip's row pitch is at least its texel data rounded up to 4 bytes.
    struct HierarchyBufferLayout {
        /// PackedHierarchy-equivalent extent of mip 0 and the number of mips.
        u32 width = 0;
        u32 height = 0;
        u32 mip_count = 0;
        u32 channels = 0;
        u32 bytes_per_channel = 0;
        vector<u64> mip_offset_bytes;
        vector<u32> mip_row_pitch_bytes;
        u64 total_bytes = 0;

        [[nodiscard]] u32 mip_width(u32 mip) const noexcept;
        [[nodiscard]] u32 mip_height(u32 mip) const noexcept;
    };

    /// `row_pitch_alignment` 4 = tightest word-aligned layout; 256 = legal buffer->texture copy source on
    /// D3D12/WebGPU (with `offset_alignment` 512 for D3D12). Both must be powers of two and multiples of 4.
    [[nodiscard]] HierarchyBufferLayout hierarchy_buffer_layout(u32 width, u32 height, HierarchyPrecision precision,
                                                                HierarchyChannels channels,
                                                                u32 row_pitch_alignment = 4,
                                                                u32 offset_alignment = 4);

    struct HierarchyBuildPass {
        HierarchyBuildConstants constants;
        u32 group_count_x = 0;
        u32 group_count_y = 0;
    };

    /// Texel rectangle of the height texture that changed, half-open (same convention as
    /// HeightfieldHierarchy::update_region).
    struct HierarchyDirtyTexels {
        u32 x0 = 0;
        u32 y0 = 0;
        u32 x1 = 0;
        u32 y1 = 0;
    };

    /// Full build (`dirty` empty): one whole-mip rectangle per level, so the power-of-two padding is zeroed
    /// too. Dirty build: per level, only the nodes hierarchy_dirty_ranges names, split at the wrap seam
    /// into non-wrapping rectangles (levels that end up with nothing to do emit no pass). Returns no passes
    /// for an invalid size.
    [[nodiscard]] vector<HierarchyBuildPass> plan_hierarchy_build(const HierarchyBufferLayout &layout, u32 width,
                                                                  u32 height, b8 wrap, HierarchyPrecision precision,
                                                                  HierarchyChannels channels,
                                                                  optional<HierarchyDirtyTexels> dirty = {});

    /// CPU execution of the shader over a pass list. `words` is the build buffer (layout.total_bytes / 4
    /// words); passes are run strictly in order, as barriers guarantee on the GPU.
    void run_hierarchy_build_cpu_model(span<const HierarchyBuildPass> passes, const HeightfieldView &heightfield,
                                       span<u32> words);

    /// Re-lays a build buffer out as HeightfieldHierarchy::pack() does (tight rows, its mip offsets), for
    /// byte-for-byte comparison. `words` is the buffer as bytes-in-words (little-endian host assumed).
    [[nodiscard]] vector<u8> repack_tight(const HierarchyBufferLayout &layout, span<const u8> buffer_bytes);

} // namespace SFT::Renderer::Displacement
