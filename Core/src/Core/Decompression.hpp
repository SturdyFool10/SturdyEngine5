#pragma once

#include <Foundation/src/Foundation.hpp>

#include <RHI/RHI.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace SFT::Core {

    // CPU-side GDeflate decompression. This is the shared decompression stage for the portable
    // texture-streaming fallback tier and the Linux io_uring backend (both fast-read-then-CPU-decode
    // paths), and Windows' own fallback when DirectStorage's GPU-side decompressor is unavailable at
    // runtime. It is NOT used by Windows' DirectStorage fast path when that path is actually taken --
    // DirectStorage decompresses directly onto a D3D12 resource, bypassing a CPU decompress step
    // entirely (see the DirectStorage backend's own doc comments).
    //
    // `decompressed_size` must be the exact original (pre-compression) byte count -- GDeflate's
    // container does not self-describe it the way e.g. zlib's does, so callers must have retained it
    // from whatever produced `compressed` (the texture-streaming asset container, see
    // Engine::TextureCompression's GDeflate sibling-file format).
    [[nodiscard]] RHI::RhiExpected<std::vector<std::byte>> decompress_gdeflate(
        std::span<const std::byte> compressed, u64 decompressed_size);

    // Compress-side counterpart, used offline/at-cache-write-time by
    // Engine::Detail::compress_gdeflate_sibling (not by the Windows DirectStorage streaming path,
    // which never needs to compress -- only Windows' own CPU fallback and Linux's io_uring path
    // decompress what this produces). `level` is GDeflate's own 1-12 scale (see the vendored
    // GDeflate.h's MinimumCompressionLevel/MaximumCompressionLevel).
    [[nodiscard]] RHI::RhiExpected<std::vector<std::byte>> compress_gdeflate(
        std::span<const std::byte> data, u32 level = 9);

} // namespace SFT::Core
