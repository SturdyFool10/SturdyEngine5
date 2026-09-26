#include <Renderer/Displacement/HeightfieldGather.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Renderer::Displacement {

    namespace {

        [[nodiscard]] f32 fetch(const HeightfieldView &hf, i32 x, i32 y, SamplerAddress sampler) noexcept {
            const auto w = static_cast<i32>(hf.width);
            const auto h = static_cast<i32>(hf.height);
            if (sampler == SamplerAddress::Repeat) {
                x = ((x % w) + w) % w;
                y = ((y % h) + h) % h;
            } else {
                x = std::clamp(x, 0, w - 1);
                y = std::clamp(y, 0, h - 1);
            }
            return hf.heights[static_cast<usize>(y) * hf.width + static_cast<usize>(x)];
        }

        [[nodiscard]] CellTexels unpack(const Gather4 &g) noexcept { return CellTexels{g.w, g.z, g.x, g.y}; }

    } // namespace

    Gather4 gather_red(const HeightfieldView &hf, f32 u, f32 v, SamplerAddress sampler) noexcept {
        const auto i0 = static_cast<i32>(std::floor(u * static_cast<f32>(hf.width) - 0.5f));
        const auto j0 = static_cast<i32>(std::floor(v * static_cast<f32>(hf.height) - 0.5f));
        return Gather4{fetch(hf, i0, j0 + 1, sampler), fetch(hf, i0 + 1, j0 + 1, sampler),
                       fetch(hf, i0 + 1, j0, sampler), fetch(hf, i0, j0, sampler)};
    }

    bool gather_cell_is_interior(i32 cx, i32 cy, u32 width, u32 height) noexcept {
        return cx >= 0 && cy >= 0 && cx <= static_cast<i32>(width) - 2 && cy <= static_cast<i32>(height) - 2;
    }

    CellTexels load_cell_direct(const HeightfieldView &hf, i32 cx, i32 cy) noexcept {
        return CellTexels{hf.texel(cx, cy), hf.texel(cx + 1, cy), hf.texel(cx, cy + 1), hf.texel(cx + 1, cy + 1)};
    }

    CellTexels load_cell_naive_gather(const HeightfieldView &hf, i32 cx, i32 cy, SamplerAddress sampler) noexcept {
        return unpack(gather_red(hf, static_cast<f32>(cx + 1) / static_cast<f32>(hf.width),
                                 static_cast<f32>(cy + 1) / static_cast<f32>(hf.height), sampler));
    }

    CellTexels load_cell_via_gather(const HeightfieldView &hf, i32 cx, i32 cy, SamplerAddress sampler) noexcept {
        if (gather_cell_is_interior(cx, cy, hf.width, hf.height)) {
            return load_cell_naive_gather(hf, cx, cy, sampler);
        }
        return load_cell_direct(hf, cx, cy);
    }

} // namespace SFT::Renderer::Displacement
