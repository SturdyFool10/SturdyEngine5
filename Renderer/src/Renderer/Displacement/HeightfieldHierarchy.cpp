#include <Renderer/Displacement/HeightfieldHierarchy.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace SFT::Renderer::Displacement {

    namespace {

        [[nodiscard]] u32 ceil_div(u32 a, u32 b) noexcept { return (a + b - 1u) / b; }

        [[nodiscard]] i32 floor_div(i32 a, i32 b) noexcept {
            i32 q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) {
                --q;
            }
            return q;
        }

        [[nodiscard]] i32 wrap_index(i32 v, i32 n) noexcept {
            const i32 m = v % n;
            return m < 0 ? m + n : m;
        }

        [[nodiscard]] u32 level_extent(u32 extent, u32 level) noexcept {
            return std::max(1u, ceil_div(extent, 1u << level));
        }

        [[nodiscard]] u32 pow2_at_least(u32 v) noexcept { return std::bit_ceil(std::max(v, 1u)); }

        [[nodiscard]] u32 packed_mip_count(u32 width, u32 height) noexcept {
            return static_cast<u32>(std::bit_width(std::max(width, height)));
        }

    } // namespace

    f32 HeightfieldView::texel(i32 x, i32 y) const noexcept {
        const auto w = static_cast<i32>(width);
        const auto h = static_cast<i32>(height);
        if (wrap) {
            x = wrap_index(x, w);
            y = wrap_index(y, h);
        } else {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
        }
        return heights[static_cast<usize>(y) * width + static_cast<usize>(x)];
    }

    void HeightfieldHierarchy::compute_leaf(const HeightfieldView &hf, u32 x, u32 y) noexcept {
        const auto xi = static_cast<i32>(x);
        const auto yi = static_cast<i32>(y);
        const f32 h00 = hf.texel(xi, yi);
        const f32 h10 = hf.texel(xi + 1, yi);
        const f32 h01 = hf.texel(xi, yi + 1);
        const f32 h11 = hf.texel(xi + 1, yi + 1);
        Level &leaf = levels_[0];
        leaf.nodes[static_cast<usize>(y) * leaf.width + x] =
            HierarchyNode{std::min({h00, h10, h01, h11}), std::max({h00, h10, h01, h11})};
    }

    void HeightfieldHierarchy::compute_parent(u32 level, u32 x, u32 y) noexcept {
        const Level &child = levels_[level - 1];
        HierarchyNode merged{1.0e30f, -1.0e30f};
        for (u32 dy = 0; dy < 2; ++dy) {
            for (u32 dx = 0; dx < 2; ++dx) {
                const u32 cx = x * 2 + dx;
                const u32 cy = y * 2 + dy;
                if (cx >= child.width || cy >= child.height) {
                    continue; // odd-sized level: the last parent has fewer than four children
                }
                const HierarchyNode &c = child.nodes[static_cast<usize>(cy) * child.width + cx];
                merged.min_height = std::min(merged.min_height, c.min_height);
                merged.max_height = std::max(merged.max_height, c.max_height);
            }
        }
        Level &parent = levels_[level];
        parent.nodes[static_cast<usize>(y) * parent.width + x] = merged;
    }

    HeightfieldHierarchy HeightfieldHierarchy::build(const HeightfieldView &heightfield) {
        HeightfieldHierarchy h;
        if (!heightfield.valid()) {
            return h;
        }
        h.wrap_ = heightfield.wrap;

        u32 level = 0;
        while (true) {
            Level l;
            l.width = level_extent(heightfield.width, level);
            l.height = level_extent(heightfield.height, level);
            l.nodes.resize(static_cast<usize>(l.width) * l.height);
            const bool top = l.width == 1 && l.height == 1;
            h.levels_.push_back(std::move(l));
            if (top) {
                break;
            }
            ++level;
        }

        for (u32 y = 0; y < h.levels_[0].height; ++y) {
            for (u32 x = 0; x < h.levels_[0].width; ++x) {
                h.compute_leaf(heightfield, x, y);
            }
        }
        for (u32 lv = 1; lv < h.level_count(); ++lv) {
            for (u32 y = 0; y < h.levels_[lv].height; ++y) {
                for (u32 x = 0; x < h.levels_[lv].width; ++x) {
                    h.compute_parent(lv, x, y);
                }
            }
        }
        return h;
    }

    u32 hierarchy_level_count(u32 width, u32 height) noexcept {
        u32 levels = 1;
        while (level_extent(width, levels - 1) > 1 || level_extent(height, levels - 1) > 1) {
            ++levels;
        }
        return levels;
    }

    vector<HierarchyDirtyRange> hierarchy_dirty_ranges(u32 width, u32 height, b8 wrap, u32 x0, u32 y0, u32 x1,
                                                       u32 y1) {
        const u32 levels = hierarchy_level_count(width, height);
        vector<HierarchyDirtyRange> out(levels);
        if (width == 0 || height == 0) {
            return {};
        }
        x1 = std::min(x1, width);
        y1 = std::min(y1, height);
        if (x0 >= x1 || y0 >= y1) {
            return out;
        }

        // Cell (i, j) reads texels (i..i+1, j..j+1), so an edited texel range [x0, x1) dirties cells
        // [x0 - 1, x1 - 1]. The upper bound is widened to x1: under clamp addressing the last cell
        // reads the last texel twice, and one redundant recompute elsewhere is harmless. Kept as
        // signed, un-wrapped bounds: at the seam the range legitimately reaches -1, which aliases
        // cell width-1 under wrap_index.
        i32 lo_x = static_cast<i32>(x0) - 1;
        i32 hi_x = static_cast<i32>(x1);
        i32 lo_y = static_cast<i32>(y0) - 1;
        i32 hi_y = static_cast<i32>(y1);

        for (u32 level = 0; level < levels; ++level) {
            const auto w = static_cast<i32>(level_extent(width, level));
            const auto h = static_cast<i32>(level_extent(height, level));
            if (!wrap) {
                lo_x = std::max(lo_x, 0);
                lo_y = std::max(lo_y, 0);
                hi_x = std::min(hi_x, w - 1);
                hi_y = std::min(hi_y, h - 1);
            }
            out[level] = HierarchyDirtyRange{lo_x, hi_x, lo_y, hi_y};

            lo_x = floor_div(lo_x, 2);
            hi_x = floor_div(hi_x, 2);
            lo_y = floor_div(lo_y, 2);
            hi_y = floor_div(hi_y, 2);
        }
        return out;
    }

    void HeightfieldHierarchy::update_region(const HeightfieldView &heightfield, u32 x0, u32 y0, u32 x1, u32 y1) {
        if (!valid() || !heightfield.valid() || x0 >= x1 || y0 >= y1) {
            return;
        }
        const vector<HierarchyDirtyRange> ranges =
            hierarchy_dirty_ranges(heightfield.width, heightfield.height, wrap_, x0, y0, x1, y1);

        for (u32 level = 0; level < level_count(); ++level) {
            const auto w = static_cast<i32>(levels_[level].width);
            const auto h = static_cast<i32>(levels_[level].height);
            const HierarchyDirtyRange &r = ranges[level];

            // A range that spans the whole level would revisit nodes under wrap; cap it.
            const i32 span_x = std::min(r.hi_x - r.lo_x + 1, w);
            const i32 span_y = std::min(r.hi_y - r.lo_y + 1, h);
            for (i32 dy = 0; dy < span_y; ++dy) {
                for (i32 dx = 0; dx < span_x; ++dx) {
                    i32 x = r.lo_x + dx;
                    i32 y = r.lo_y + dy;
                    if (wrap_) {
                        x = wrap_index(x, w);
                        y = wrap_index(y, h);
                    } else {
                        if (x < 0 || x >= w || y < 0 || y >= h) {
                            continue;
                        }
                    }
                    if (level == 0) {
                        compute_leaf(heightfield, static_cast<u32>(x), static_cast<u32>(y));
                    } else {
                        compute_parent(level, static_cast<u32>(x), static_cast<u32>(y));
                    }
                }
            }
        }
    }

    PackedHierarchy HeightfieldHierarchy::pack(HierarchyPrecision precision, HierarchyChannels channels) const {
        PackedHierarchy out;
        if (!valid()) {
            return out;
        }
        // Level 0 is not stored: traversal solves leaf cells from a four-texel gather of the height texture
        // and bounds them with that gather's own max, so a level-0 node would be a redundant read. Dropping
        // it cuts the resource to a quarter. Packed mip m therefore holds hierarchy level m + 1.
        const u32 full_w = pow2_at_least(levels_[0].width);
        const u32 full_h = pow2_at_least(levels_[0].height);
        out.width = std::max(1u, full_w >> 1);
        out.height = std::max(1u, full_h >> 1);
        out.mip_count = std::max(1u, packed_mip_count(full_w, full_h) - 1u);
        out.channels = channels == HierarchyChannels::MinMax ? 2u : 1u;
        out.bytes_per_channel = precision == HierarchyPrecision::Float32 ? 4u : 2u;

        u64 total = 0;
        for (u32 mip = 0; mip < out.mip_count; ++mip) {
            out.mip_offsets.push_back(total);
            const u64 w = std::max(1u, out.width >> mip);
            const u64 h = std::max(1u, out.height >> mip);
            total += w * h * out.channels * out.bytes_per_channel;
        }
        out.data.assign(static_cast<usize>(total), 0);

        for (u32 mip = 0; mip < out.mip_count && mip + 1 < level_count(); ++mip) {
            const Level &level = levels_[mip + 1];
            const u32 mip_w = std::max(1u, out.width >> mip);
            u8 *base = out.data.data() + out.mip_offsets[mip];
            for (u32 y = 0; y < level.height; ++y) {
                for (u32 x = 0; x < level.width; ++x) {
                    const HierarchyNode &n = level.nodes[static_cast<usize>(y) * level.width + x];
                    u8 *texel = base + (static_cast<usize>(y) * mip_w + x) * out.channels * out.bytes_per_channel;

                    // Channel order is (min, max) so a single-channel hierarchy is just "max" in R.
                    auto store = [&](u32 channel, f32 value, bool round_up) {
                        u8 *dst = texel + channel * out.bytes_per_channel;
                        if (precision == HierarchyPrecision::Float32) {
                            std::memcpy(dst, &value, sizeof(f32));
                        } else {
                            const f32 scaled = std::clamp(value, 0.0f, 1.0f) * 65535.0f;
                            const f32 rounded = round_up ? std::ceil(scaled) : std::floor(scaled);
                            const auto q = static_cast<u16>(std::clamp(rounded, 0.0f, 65535.0f));
                            std::memcpy(dst, &q, sizeof(u16));
                        }
                    };
                    if (channels == HierarchyChannels::MinMax) {
                        store(0, n.min_height, false);
                        store(1, n.max_height, true);
                    } else {
                        store(0, n.max_height, true);
                    }
                }
            }
        }
        return out;
    }

    u64 hierarchy_memory_bytes(u32 width, u32 height, HierarchyPrecision precision,
                               HierarchyChannels channels) noexcept {
        const u32 full_w = pow2_at_least(width);
        const u32 full_h = pow2_at_least(height);
        const u32 w = std::max(1u, full_w >> 1);
        const u32 h = std::max(1u, full_h >> 1);
        const u32 mips = std::max(1u, packed_mip_count(full_w, full_h) - 1u);
        const u64 texel_bytes = static_cast<u64>(channels == HierarchyChannels::MinMax ? 2u : 1u) *
                                (precision == HierarchyPrecision::Float32 ? 4u : 2u);
        u64 total = 0;
        for (u32 mip = 0; mip < mips; ++mip) {
            total += static_cast<u64>(std::max(1u, w >> mip)) * std::max(1u, h >> mip) * texel_bytes;
        }
        return total;
    }

} // namespace SFT::Renderer::Displacement
