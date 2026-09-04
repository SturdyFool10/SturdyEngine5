/// Covers `pack_texture_upload_rows` on its own, CPU-only: it and `Renderer::submit_texture_upload`
/// (RendererTextures.cpp) have to agree byte-for-byte on the padded layout without either directly
/// calling the other, so a change to one that silently drifts from the other would only show up as
/// visually wrong pixels on a real D3D12 or WebGPU run. This reconstructs `submit_texture_upload`'s
/// own copy-region formula independently and checks it recovers exactly what was packed.

#include <Renderer/TextureUploadPacking.hpp>

#include <RHI/RHI.hpp>

#include <cstring>
#include <iostream>
#include <vector>

namespace {

    namespace rhi = SFT::RHI;
    using SFT::Renderer::backend_requires_padded_texture_rows;
    using SFT::Renderer::pack_texture_upload_rows;

    /// Checks the supplied condition and reports the accompanying diagnostic message when it is false.
    ///
    /// @param condition Condition controlling whether the operation proceeds.
    /// @param message Text consumed by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    /// `RendererTextures.cpp::submit_texture_upload`'s own row-pitch and level-offset formula,
    /// reimplemented here rather than shared, so this test independently confirms the two agree
    /// instead of trivially calling into the same code twice.
    ///
    /// @param bytes_per_texel Bytes one texel of the format occupies.
    /// @param level_width Pixel width of the level.
    ///
    /// @return Returns the padded row pitch in bytes.
    /// @note This function does not throw exceptions.
    u64 expected_row_pitch(u32 bytes_per_texel, u32 level_width) {
        const u64 tight = static_cast<u64>(level_width) * bytes_per_texel;
        return (tight + 255u) & ~u64{255u};
    }

    /// Verifies that packing a small RGBA8 mip chain, then reading it back with
    /// `submit_texture_upload`'s own formula, recovers exactly the original tight bytes.
    ///
    /// Level 0 is 50x7: the width is not a multiple of 64 texels, so the tight row (200 bytes) and
    /// the padded row (256 bytes) differ and a naive width-scaled pitch would be wrong; the height
    /// is chosen so the level-0 byte count (1792) is not itself a multiple of 512, so a wrong
    /// inter-level alignment produces a detectably different offset rather than coincidentally
    /// landing on the same one.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool round_trips_a_multi_level_rgba8_chain() {
        constexpr u32 width = 50;
        constexpr u32 height = 7;
        constexpr u32 mip_levels = 3; // 50x8, 25x4, 12x2
        constexpr u32 bytes_per_texel = 4;

        std::vector<std::byte> tight;
        u32 level_width = width;
        u32 level_height = height;
        std::vector<std::pair<u64, u32>> level_offsets; // (offset into `tight`, width) per level
        for (u32 level = 0; level < mip_levels; ++level) {
            level_offsets.emplace_back(tight.size(), level_width);
            const usize level_bytes = static_cast<usize>(level_width) * level_height * bytes_per_texel;
            const usize base = tight.size();
            tight.resize(base + level_bytes);
            // Fill with a pattern that encodes (level, row, col) so a misaligned read is caught
            // rather than coincidentally matching zero-fill.
            for (u32 row = 0; row < level_height; ++row) {
                for (u32 col = 0; col < level_width; ++col) {
                    const usize texel = base + (static_cast<usize>(row) * level_width + col) * bytes_per_texel;
                    tight[texel + 0] = static_cast<std::byte>(level);
                    tight[texel + 1] = static_cast<std::byte>(row);
                    tight[texel + 2] = static_cast<std::byte>(col);
                    tight[texel + 3] = std::byte{0xFF};
                }
            }
            level_width = std::max(level_width / 2u, 1u);
            level_height = std::max(level_height / 2u, 1u);
        }

        const std::vector<std::byte> padded = pack_texture_upload_rows(
            rhi::Format::RGBA8Unorm, width, height, mip_levels,
            std::span<const std::byte>{tight.data(), tight.size()});

        bool ok = true;
        u64 destination_offset = 0;
        level_width = width;
        level_height = height;
        for (u32 level = 0; level < mip_levels; ++level) {
            destination_offset = (destination_offset + 511u) & ~u64{511u};
            const u64 row_pitch = expected_row_pitch(bytes_per_texel, level_width);
            const auto &[tight_level_offset, tight_level_width] = level_offsets[level];
            ok = check(tight_level_width == level_width, "test's own level-offset bookkeeping is consistent") && ok;

            for (u32 row = 0; row < level_height; ++row) {
                const u64 padded_row_offset = destination_offset + row * row_pitch;
                const usize tight_row_offset =
                    tight_level_offset + static_cast<usize>(row) * level_width * bytes_per_texel;
                const usize row_bytes = static_cast<usize>(level_width) * bytes_per_texel;
                if (padded_row_offset + row_bytes > padded.size()) {
                    ok = check(false, "a packed row falls outside the buffer pack_texture_upload_rows returned") && ok;
                    continue;
                }
                const int mismatch =
                    std::memcmp(padded.data() + padded_row_offset, tight.data() + tight_row_offset, row_bytes);
                ok = check(mismatch == 0,
                          "a packed row's bytes do not match the tight source at the offset "
                          "submit_texture_upload's own formula computes") &&
                     ok;
            }
            destination_offset += row_pitch * level_height;
            level_width = std::max(level_width / 2u, 1u);
            level_height = std::max(level_height / 2u, 1u);
        }
        return ok;
    }

    /// Verifies the backend list itself: exactly D3D12 and WebGPU need padding, matching what each
    /// backend's own command encoder enforces (D3D12's native row-pitch rule; WebGPU's identical
    /// one in `WebGpuCommandEncoder.cpp`'s `texel_copy_layout`).
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    bool only_d3d12_and_webgpu_require_padding() {
        bool ok = true;
        ok = check(backend_requires_padded_texture_rows(rhi::BackendType::D3D12), "D3D12 requires padded rows") && ok;
        ok = check(backend_requires_padded_texture_rows(rhi::BackendType::WebGpu), "WebGPU requires padded rows") && ok;
        ok = check(!backend_requires_padded_texture_rows(rhi::BackendType::Vulkan),
                  "Vulkan does not require padded rows") &&
             ok;
        ok = check(!backend_requires_padded_texture_rows(rhi::BackendType::Metal),
                  "Metal does not require padded rows") &&
             ok;
        return ok;
    }

} // namespace

/// Runs the executable entry point and returns its process exit status.
///
/// @return Returns the process/application exit status; zero conventionally indicates successful completion.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
int main() {
    const bool passed = round_trips_a_multi_level_rgba8_chain() && only_d3d12_and_webgpu_require_padding();
    return passed ? 0 : 1;
}
