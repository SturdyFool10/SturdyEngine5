#pragma once

#include <Foundation/Foundation.hpp>

#include <Renderer/Displacement/HeightfieldHierarchy.hpp>

// CPU model of the shader's corner gather (SFT_HF_USE_GATHER in Shaders/sturdy_heightfield.slang): what a
// hardware four-texel gather returns, in which component order, under which sampler address mode — and the
// interior/border rule that makes the shader's result independent of that address mode. It exists so the
// component order and the wrap/clamp parity claim are unit-tested without a GPU, and so a GPU comparison has
// something to be compared with.
namespace SFT::Renderer::Displacement {

    /// Address mode of the sampler the height texture happens to be bound with. The shader cannot see it.
    enum class SamplerAddress : u8 {
        Repeat,
        ClampToEdge,
    };

    /// Result of GatherRed. With i0 = floor(u*W - 0.5), j0 = floor(v*H - 0.5) and i1 = i0 + 1, j1 = j0 + 1:
    /// x = (i0, j1), y = (i1, j1), z = (i1, j0), w = (i0, j0) — the same order in D3D, Vulkan and WGSL.
    struct Gather4 {
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 z = 0.0f;
        f32 w = 0.0f;
    };

    /// The four texels of one bilinear cell in the order the trace code names them.
    struct CellTexels {
        f32 h00 = 0.0f; // (cx,     cy)
        f32 h10 = 0.0f; // (cx + 1, cy)
        f32 h01 = 0.0f; // (cx,     cy + 1)
        f32 h11 = 0.0f; // (cx + 1, cy + 1)
    };

    /// A hardware GatherRed at (u, v) in tile units. Texel coordinates outside the texture are resolved by
    /// `sampler` — this is *not* the view's addressing, exactly as on a GPU where the sampler is bound
    /// separately from the shader's own `wrap` setting.
    [[nodiscard]] Gather4 gather_red(const HeightfieldView &heightfield, f32 u, f32 v,
                                     SamplerAddress sampler) noexcept;

    /// True when all four texels of cell (cx, cy) are inside the texture, i.e. the gather's address mode
    /// cannot matter. Only such cells may use the gather.
    [[nodiscard]] bool gather_cell_is_interior(i32 cx, i32 cy, u32 width, u32 height) noexcept;

    /// The shader's hf_cell under SFT_HF_USE_GATHER: one gather at the shared corner ((cx + 1) / W,
    /// (cy + 1) / H) for interior cells; explicit wrap/clamp reads (HeightfieldView::texel — the shader's
    /// Load()s) elsewhere. Equals the four-Load() result for every cell and either `sampler`.
    [[nodiscard]] CellTexels load_cell_via_gather(const HeightfieldView &heightfield, i32 cx, i32 cy,
                                                  SamplerAddress sampler) noexcept;

    /// Reference: the four explicit reads (the baseline hf_cell).
    [[nodiscard]] CellTexels load_cell_direct(const HeightfieldView &heightfield, i32 cx, i32 cy) noexcept;

    /// The rejected design — gather everywhere and trust the sampler. Correct only when the sampler's
    /// address mode matches the view's; kept so a test can prove why the interior rule is needed.
    [[nodiscard]] CellTexels load_cell_naive_gather(const HeightfieldView &heightfield, i32 cx, i32 cy,
                                                    SamplerAddress sampler) noexcept;

} // namespace SFT::Renderer::Displacement
