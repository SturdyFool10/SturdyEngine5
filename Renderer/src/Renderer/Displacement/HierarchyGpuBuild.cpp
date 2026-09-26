#include <Renderer/Displacement/HierarchyGpuBuild.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace SFT::Renderer::Displacement {

    namespace {

        [[nodiscard]] u32 ceil_div(u32 a, u32 b) noexcept { return (a + b - 1u) / b; }
        [[nodiscard]] u64 align_up(u64 v, u64 a) noexcept { return (v + a - 1u) / a * a; }
        [[nodiscard]] u32 level_extent(u32 extent, u32 level) noexcept {
            return std::max(1u, ceil_div(extent, 1u << level));
        }

        [[nodiscard]] i32 wrap_index(i32 v, i32 n) noexcept {
            const i32 m = v % n;
            return m < 0 ? m + n : m;
        }

        struct Rect {
            u32 x0, y0, x1, y1;
        };

        /// Splits an inclusive signed range on an axis of length n into 1-2 half-open [a, b) intervals.
        void split_axis(i32 lo, i32 hi, i32 n, bool wrap, vector<std::pair<u32, u32>> &out) {
            if (lo > hi) {
                return;
            }
            const i32 span = std::min(hi - lo + 1, n);
            const i32 start = wrap ? wrap_index(lo, n) : lo;
            if (start + span <= n) {
                out.emplace_back(static_cast<u32>(start), static_cast<u32>(start + span));
            } else {
                out.emplace_back(static_cast<u32>(start), static_cast<u32>(n));
                out.emplace_back(0u, static_cast<u32>(start + span - n));
            }
        }

        [[nodiscard]] u32 words_begin(u32 format, u32 rect_x0) noexcept {
            const bool f32 = (format & kHierarchyFormatFloat32) != 0;
            const bool two = (format & kHierarchyFormatTwoChannels) != 0;
            if (!f32 && !two) {
                return rect_x0 >> 1;
            }
            return (f32 && two) ? rect_x0 * 2 : rect_x0;
        }

        [[nodiscard]] u32 words_end(u32 format, u32 rect_x1) noexcept {
            const bool f32 = (format & kHierarchyFormatFloat32) != 0;
            const bool two = (format & kHierarchyFormatTwoChannels) != 0;
            if (!f32 && !two) {
                return (rect_x1 + 1) >> 1;
            }
            return (f32 && two) ? rect_x1 * 2 : rect_x1;
        }

        // ---- CPU model of the shader ----------------------------------------------------------------

        struct Bounds {
            f32 min_f = 0.0f;
            f32 max_f = 0.0f;
            u32 min_q = 0;
            u32 max_q = 0;
        };

        [[nodiscard]] u32 quant_up(f32 v) noexcept {
            return static_cast<u32>(std::min(std::ceil(std::clamp(v, 0.0f, 1.0f) * 65535.0f), 65535.0f));
        }
        [[nodiscard]] u32 quant_down(f32 v) noexcept {
            return static_cast<u32>(std::min(std::floor(std::clamp(v, 0.0f, 1.0f) * 65535.0f), 65535.0f));
        }

        [[nodiscard]] f32 as_float(u32 w) noexcept { return std::bit_cast<f32>(w); }

        [[nodiscard]] Bounds read_child(const HierarchyBuildConstants &c, const HeightfieldView &hf,
                                        span<const u32> words, u32 x, u32 y) noexcept {
            Bounds b;
            if (c.level == 1) {
                const auto xi = static_cast<i32>(x);
                const auto yi = static_cast<i32>(y);
                const f32 h00 = hf.texel(xi, yi);
                const f32 h10 = hf.texel(xi + 1, yi);
                const f32 h01 = hf.texel(xi, yi + 1);
                const f32 h11 = hf.texel(xi + 1, yi + 1);
                b.min_f = std::min(std::min(h00, h10), std::min(h01, h11));
                b.max_f = std::max(std::max(h00, h10), std::max(h01, h11));
                return b;
            }
            const u32 row = c.in_offset_words + y * c.in_pitch_words;
            const bool two = (c.format & kHierarchyFormatTwoChannels) != 0;
            if ((c.format & kHierarchyFormatFloat32) != 0) {
                if (two) {
                    b.min_f = as_float(words[row + 2 * x]);
                    b.max_f = as_float(words[row + 2 * x + 1]);
                } else {
                    b.max_f = as_float(words[row + x]);
                }
            } else if (two) {
                const u32 w = words[row + x];
                b.min_q = w & 0xFFFFu;
                b.max_q = w >> 16;
            } else {
                const u32 w = words[row + (x >> 1)];
                b.max_q = (w >> ((x & 1u) * 16u)) & 0xFFFFu;
            }
            return b;
        }

        [[nodiscard]] Bounds compute_node(const HierarchyBuildConstants &c, const HeightfieldView &hf,
                                          span<const u32> words, u32 x, u32 y) noexcept {
            Bounds acc{1.0e30f, -1.0e30f, 0xFFFFu, 0u};
            for (u32 dy = 0; dy < 2; ++dy) {
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u32 cx = x * 2 + dx;
                    const u32 cy = y * 2 + dy;
                    if (cx >= c.child_width || cy >= c.child_height) {
                        continue;
                    }
                    const Bounds ch = read_child(c, hf, words, cx, cy);
                    acc.min_f = std::min(acc.min_f, ch.min_f);
                    acc.max_f = std::max(acc.max_f, ch.max_f);
                    acc.min_q = std::min(acc.min_q, ch.min_q);
                    acc.max_q = std::max(acc.max_q, ch.max_q);
                }
            }
            if (c.level == 1 && (c.format & kHierarchyFormatFloat32) == 0) {
                acc.min_q = quant_down(acc.min_f);
                acc.max_q = quant_up(acc.max_f);
            }
            return acc;
        }

        [[nodiscard]] u32 texel_word16(const HierarchyBuildConstants &c, const HeightfieldView &hf,
                                       span<const u32> words, u32 x, u32 y, bool two) noexcept {
            if (x >= c.level_width || y >= c.level_height) {
                return 0;
            }
            const Bounds b = compute_node(c, hf, words, x, y);
            return two ? (b.min_q | (b.max_q << 16)) : b.max_q;
        }

        void run_thread(const HierarchyBuildConstants &c, const HeightfieldView &hf, span<u32> words, u32 tid_x,
                        u32 tid_y) noexcept {
            const bool is_float = (c.format & kHierarchyFormatFloat32) != 0;
            const bool two = (c.format & kHierarchyFormatTwoChannels) != 0;
            const u32 wx = words_begin(c.format, c.rect_x0) + tid_x;
            const u32 y = c.rect_y0 + tid_y;
            if (wx >= words_end(c.format, c.rect_x1) || y >= c.rect_y1) {
                return;
            }
            u32 word = 0;
            if (!is_float) {
                if (two) {
                    word = texel_word16(c, hf, words, wx, y, true);
                } else {
                    const u32 lo = texel_word16(c, hf, words, wx * 2, y, false);
                    const u32 hi = texel_word16(c, hf, words, wx * 2 + 1, y, false);
                    word = lo | (hi << 16);
                }
            } else {
                const u32 tx = two ? (wx >> 1) : wx;
                if (tx < c.level_width && y < c.level_height) {
                    const Bounds b = compute_node(c, hf, words, tx, y);
                    word = std::bit_cast<u32>((two && (wx & 1u) == 0) ? b.min_f : b.max_f);
                }
            }
            words[c.out_offset_words + y * c.out_pitch_words + wx] = word;
        }

    } // namespace

    u32 HierarchyBufferLayout::mip_width(u32 mip) const noexcept { return std::max(1u, width >> mip); }
    u32 HierarchyBufferLayout::mip_height(u32 mip) const noexcept { return std::max(1u, height >> mip); }

    HierarchyBufferLayout hierarchy_buffer_layout(u32 width, u32 height, HierarchyPrecision precision,
                                                  HierarchyChannels channels, u32 row_pitch_alignment,
                                                  u32 offset_alignment) {
        HierarchyBufferLayout l;
        if (width == 0 || height == 0) {
            return l;
        }
        // Same arithmetic as HeightfieldHierarchy::pack().
        const u32 full_w = std::bit_ceil(width);
        const u32 full_h = std::bit_ceil(height);
        l.width = std::max(1u, full_w >> 1);
        l.height = std::max(1u, full_h >> 1);
        l.mip_count = std::max(1u, static_cast<u32>(std::bit_width(std::max(full_w, full_h))) - 1u);
        l.channels = channels == HierarchyChannels::MinMax ? 2u : 1u;
        l.bytes_per_channel = precision == HierarchyPrecision::Float32 ? 4u : 2u;

        u64 total = 0;
        for (u32 mip = 0; mip < l.mip_count; ++mip) {
            total = align_up(total, std::max(offset_alignment, 4u));
            l.mip_offset_bytes.push_back(total);
            const u64 row_bytes = static_cast<u64>(l.mip_width(mip)) * l.channels * l.bytes_per_channel;
            const auto pitch = static_cast<u32>(align_up(row_bytes, std::max(row_pitch_alignment, 4u)));
            l.mip_row_pitch_bytes.push_back(pitch);
            total += static_cast<u64>(pitch) * l.mip_height(mip);
        }
        l.total_bytes = total;
        return l;
    }

    vector<HierarchyBuildPass> plan_hierarchy_build(const HierarchyBufferLayout &layout, u32 width, u32 height,
                                                    b8 wrap, HierarchyPrecision precision,
                                                    HierarchyChannels channels, optional<HierarchyDirtyTexels> dirty) {
        vector<HierarchyBuildPass> passes;
        if (width == 0 || height == 0 || layout.mip_count == 0) {
            return passes;
        }
        const u32 levels = hierarchy_level_count(width, height);
        const u32 format = (precision == HierarchyPrecision::Float32 ? kHierarchyFormatFloat32 : 0u) |
                           (channels == HierarchyChannels::MinMax ? kHierarchyFormatTwoChannels : 0u);

        vector<HierarchyDirtyRange> ranges;
        if (dirty) {
            ranges = hierarchy_dirty_ranges(width, height, wrap, dirty->x0, dirty->y0, dirty->x1, dirty->y1);
        }

        // A field whose hierarchy has fewer levels than the packed chain has mips (only 1x1: pack() still emits one
        // zero mip) gets zero-fill passes for the surplus on a full build: real level size 0 writes zeros.
        const u32 last_level = dirty ? levels : std::max(levels, layout.mip_count + 1);
        for (u32 level = 1; level < last_level && level - 1 < layout.mip_count; ++level) {
            const u32 mip = level - 1;
            const bool real_level = level < levels;
            const u32 level_w = real_level ? level_extent(width, level) : 0u;
            const u32 level_h = real_level ? level_extent(height, level) : 0u;

            vector<Rect> rects;
            if (!dirty) {
                rects.push_back(Rect{0, 0, layout.mip_width(mip), layout.mip_height(mip)});
            } else {
                const HierarchyDirtyRange &r = ranges[level];
                vector<std::pair<u32, u32>> xs;
                vector<std::pair<u32, u32>> ys;
                split_axis(r.lo_x, r.hi_x, static_cast<i32>(level_w), static_cast<bool>(wrap), xs);
                split_axis(r.lo_y, r.hi_y, static_cast<i32>(level_h), static_cast<bool>(wrap), ys);
                for (const auto &[y0, y1] : ys) {
                    for (const auto &[x0, x1] : xs) {
                        rects.push_back(Rect{x0, y0, x1, y1});
                    }
                }
            }

            for (const Rect &rect : rects) {
                HierarchyBuildConstants c;
                c.src_width = width;
                c.src_height = height;
                c.wrap = wrap ? 1u : 0u;
                c.format = format;
                c.level = level;
                c.level_width = level_w;
                c.level_height = level_h;
                c.child_width = real_level ? level_extent(width, level - 1) : 0u;
                c.child_height = real_level ? level_extent(height, level - 1) : 0u;
                c.rect_x0 = rect.x0;
                c.rect_y0 = rect.y0;
                c.rect_x1 = rect.x1;
                c.rect_y1 = rect.y1;
                c.out_offset_words = static_cast<u32>(layout.mip_offset_bytes[mip] / 4);
                c.out_pitch_words = layout.mip_row_pitch_bytes[mip] / 4;
                if (level >= 2) {
                    c.in_offset_words = static_cast<u32>(layout.mip_offset_bytes[mip - 1] / 4);
                    c.in_pitch_words = layout.mip_row_pitch_bytes[mip - 1] / 4;
                }
                HierarchyBuildPass pass;
                pass.constants = c;
                const u32 word_span = words_end(format, rect.x1) - words_begin(format, rect.x0);
                pass.group_count_x = ceil_div(word_span, 8);
                pass.group_count_y = ceil_div(rect.y1 - rect.y0, 8);
                passes.push_back(pass);
            }
        }
        return passes;
    }

    void run_hierarchy_build_cpu_model(span<const HierarchyBuildPass> passes, const HeightfieldView &heightfield,
                                       span<u32> words) {
        for (const HierarchyBuildPass &pass : passes) {
            for (u32 gy = 0; gy < pass.group_count_y * 8; ++gy) {
                for (u32 gx = 0; gx < pass.group_count_x * 8; ++gx) {
                    run_thread(pass.constants, heightfield, words, gx, gy);
                }
            }
        }
    }

    vector<u8> repack_tight(const HierarchyBufferLayout &layout, span<const u8> buffer_bytes) {
        vector<u8> out;
        u64 total = 0;
        vector<u64> tight_offsets;
        for (u32 mip = 0; mip < layout.mip_count; ++mip) {
            tight_offsets.push_back(total);
            total += static_cast<u64>(layout.mip_width(mip)) * layout.mip_height(mip) * layout.channels *
                     layout.bytes_per_channel;
        }
        out.assign(static_cast<usize>(total), 0);
        for (u32 mip = 0; mip < layout.mip_count; ++mip) {
            const u64 row_bytes = static_cast<u64>(layout.mip_width(mip)) * layout.channels * layout.bytes_per_channel;
            for (u32 y = 0; y < layout.mip_height(mip); ++y) {
                const u64 src = layout.mip_offset_bytes[mip] + static_cast<u64>(y) * layout.mip_row_pitch_bytes[mip];
                if (src + row_bytes > buffer_bytes.size()) {
                    continue;
                }
                std::memcpy(out.data() + tight_offsets[mip] + y * row_bytes, buffer_bytes.data() + src,
                            static_cast<usize>(row_bytes));
            }
        }
        return out;
    }

} // namespace SFT::Renderer::Displacement
