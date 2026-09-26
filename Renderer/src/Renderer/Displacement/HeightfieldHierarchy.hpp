#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <span>
#include <vector>
#pragma endregion

#include <Renderer/Displacement/DisplacementTypes.hpp>

using std::span;
using std::vector;

namespace SFT::Renderer::Displacement {

    /// Read-only view of a single-channel heightfield, values normalized to [0, 1]. Texel (x, y)
    /// holds the height at the *centre* of that texel, which is also where hardware bilinear
    /// filtering places its sample points, so a "cell" here — the bilinear patch between four
    /// neighbouring texel centres — is exactly the surface a filtered texture fetch would trace out.
    /// That equivalence is what lets the exact-intersection shader agree with ordinary sampling.
    struct HeightfieldView {
        span<const f32> heights;
        u32 width = 0;
        u32 height = 0;
        /// Repeat addressing. When false, out-of-range texels clamp to the edge.
        b8 wrap = true;

        [[nodiscard]] bool valid() const noexcept {
            return width > 0 && height > 0 && heights.size() >= static_cast<usize>(width) * height;
        }

        /// Height at an integer texel, applying the addressing mode. Accepts any signed coordinate.
        [[nodiscard]] f32 texel(i32 x, i32 y) const noexcept;
    };

    struct HierarchyNode {
        f32 min_height = 0.0f;
        f32 max_height = 0.0f;
    };

    /// GPU-uploadable form: a mip chain whose mip m is hierarchy level m + 1 (level 0 is not stored — see
    /// HeightfieldHierarchy::pack). Extents are rounded up to a power of two so ordinary texture mip
    /// sizing (floor(extent / 2^m)) lines up with the hierarchy's own level sizes for any source
    /// dimensions; entries beyond the real level size are zero and never read (the shader indexes with
    /// the same arithmetic that produced them, so it never leaves the real region).
    /// `width`/`height` are the extent of mip 0.
    struct PackedHierarchy {
        u32 width = 0;
        u32 height = 0;
        u32 mip_count = 0;
        /// 1 (max) or 2 (min, max) interleaved per texel.
        u32 channels = 0;
        u32 bytes_per_channel = 0;
        vector<u8> data;
        /// Byte offset of each mip inside `data`.
        vector<u64> mip_offsets;
    };

    /// Min/max bounds over bilinear cells, as an implicit quadtree (plans/displacement.md §10-11).
    ///
    /// Level 0 has one node per *cell* (width x height of them: cell (i, j) spans texel centres
    /// (i, j)..(i+1, j+1), wrapping or clamping at the edge). Level L+1 merges 2x2 nodes of level L, so
    /// a node bounds every height its whole region can reach: min <= H(u, v) <= max. Because a
    /// bilinear patch has no interior extremum (it is a saddle at worst), a cell's four corners are
    /// enough to bound it exactly.
    ///
    /// The top level is a single node. Level sizes are ceil(extent / 2^L) per axis, clamped to 1.
    class HeightfieldHierarchy {
      public:
        HeightfieldHierarchy() = default;

        /// Builds the full hierarchy. Returns an empty (valid() == false) hierarchy for an invalid view.
        [[nodiscard]] static HeightfieldHierarchy build(const HeightfieldView &heightfield);

        /// Recomputes only the nodes that depend on texels in [x0, x1) x [y0, y1) (half-open, in
        /// texel coordinates; must be inside the heightfield). The result is bit-identical to a full
        /// build() of the edited heights — `heightfield` must already hold the edited values.
        /// Cost is proportional to the region plus a one-cell border per level, not to the texture.
        void update_region(const HeightfieldView &heightfield, u32 x0, u32 y0, u32 x1, u32 y1);

        [[nodiscard]] bool valid() const noexcept { return !levels_.empty(); }
        [[nodiscard]] u32 level_count() const noexcept { return static_cast<u32>(levels_.size()); }
        [[nodiscard]] u32 level_width(u32 level) const noexcept { return levels_[level].width; }
        [[nodiscard]] u32 level_height(u32 level) const noexcept { return levels_[level].height; }
        [[nodiscard]] b8 wraps() const noexcept { return wrap_; }

        /// Node at (x, y) of `level`; coordinates must be inside that level.
        [[nodiscard]] const HierarchyNode &node(u32 level, u32 x, u32 y) const noexcept {
            const Level &l = levels_[level];
            return l.nodes[static_cast<usize>(y) * l.width + x];
        }

        /// Quantizes and lays levels 1.. out as a mip chain for upload. Quantization is
        /// conservative — min rounds down, max rounds up — so a coarse node never claims a tighter
        /// bound than the true one and the skip test can never miss a real hit.
        [[nodiscard]] PackedHierarchy pack(HierarchyPrecision precision, HierarchyChannels channels) const;

      private:
        struct Level {
            u32 width = 0;
            u32 height = 0;
            vector<HierarchyNode> nodes;
        };

        void compute_leaf(const HeightfieldView &heightfield, u32 x, u32 y) noexcept;
        void compute_parent(u32 level, u32 x, u32 y) noexcept;

        vector<Level> levels_;
        b8 wrap_ = true;
    };

    /// Inclusive, signed node range of one hierarchy level that depends on an edited texel region. Un-wrapped
    /// (at a wrapping seam `lo` may be -1 or `hi` may equal the level size, aliasing the far side); for a
    /// non-wrapping field it is already clamped into the level, and empty() ranges have lo > hi.
    struct HierarchyDirtyRange {
        i32 lo_x = 0;
        i32 hi_x = -1;
        i32 lo_y = 0;
        i32 hi_y = -1;
        [[nodiscard]] bool empty() const noexcept { return lo_x > hi_x || lo_y > hi_y; }
    };

    /// Number of levels (including the implicit level 0) a hierarchy over width x height texels has.
    [[nodiscard]] u32 hierarchy_level_count(u32 width, u32 height) noexcept;

    /// Nodes each level must recompute after texels [x0, x1) x [y0, y1) changed — the single definition
    /// shared by HeightfieldHierarchy::update_region and the GPU dirty-tile build (plan_hierarchy_build).
    /// Entry L is level L; the vector always has hierarchy_level_count() entries.
    [[nodiscard]] vector<HierarchyDirtyRange> hierarchy_dirty_ranges(u32 width, u32 height, b8 wrap, u32 x0, u32 y0,
                                                                     u32 x1, u32 y1);

    /// Bytes a packed hierarchy occupies, without building one — for the memory-budget check that
    /// decides whether HierarchicalCellExact is affordable for a given texture and tier.
    [[nodiscard]] u64 hierarchy_memory_bytes(u32 width, u32 height, HierarchyPrecision precision,
                                             HierarchyChannels channels) noexcept;

} // namespace SFT::Renderer::Displacement
