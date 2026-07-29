#pragma once

#include <Foundation/src/Foundation.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace SFT::Engine::Detail {

    // BC7-encodes `rgba8` (tightly-packed width*height*4 bytes, as validated by
    // AssetManager::create_texture) for a real VRAM win (~4x smaller than RGBA8Unorm) with no
    // shader changes anywhere (BC7 stores full RGBA, so it's a drop-in replacement for
    // RGBA8Unorm/RGBA8UnormSrgb wherever a texture is sampled). Checks/populates an on-disk cache
    // under .cache/compressed_textures/ first (keyed by a content hash of the pixels + dimensions
    // + `srgb`) so repeat loads of the same texture skip bc7enc's real encode cost, which is
    // CPU-slow at the quality level this uses.
    //
    // Returns nullopt (never a hard error) when width or height is smaller than one 4x4 block, or
    // if encoding fails for any reason — callers fall back to uploading `rgba8` uncompressed in
    // either case, exactly like a device that doesn't report
    // RHI::DeviceLimits::supports_bc_texture_compression at all.
    [[nodiscard]] std::optional<std::vector<std::byte>> compress_bc7(
        std::span<const std::byte> rgba8, u32 width, u32 height, bool srgb);

} // namespace SFT::Engine::Detail
