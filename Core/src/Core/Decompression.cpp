#include "Decompression.hpp"

#include <Foundation/src/Foundation.hpp>

// Reference GDeflate CPU decoder vendored from microsoft/DirectStorage's GDeflate/ subfolder (see
// cmake/SturdyDependencies.cmake's sturdy_fetch_gdeflate()). The exact header name/namespace/function
// signature below (GDeflate::Decompress) reflects the reference decoder's publicly documented API
// shape at the time this was written but was NOT independently verified against the live vendored
// source in this environment -- if this fails to compile once the dependency is actually fetched,
// check the real signature in the vendored GDeflate.h and adjust the call below to match; the
// surrounding decompress_gdeflate() contract (span in, exact-size buffer out, RhiExpected<vector<byte>>
// result) should not need to change.
#include <GDeflate.h>

#include <tracy/Tracy.hpp>

namespace SFT::Core {

    RHI::RhiExpected<std::vector<std::byte>> decompress_gdeflate(std::span<const std::byte> compressed,
                                                                  u64 decompressed_size) {
        ZoneScopedN("Core::decompress_gdeflate");
        if (compressed.empty() || decompressed_size == 0) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::InvalidArgument,
                                             "decompress_gdeflate: empty input or zero decompressed size."));
        }

        std::vector<std::byte> decompressed(static_cast<usize>(decompressed_size));
        const bool ok = GDeflate::Decompress(
            reinterpret_cast<uint8_t *>(decompressed.data()), decompressed.size(),
            reinterpret_cast<const uint8_t *>(compressed.data()), compressed.size(),
            /*numWorkerThreads=*/1);
        if (!ok) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                             "decompress_gdeflate: GDeflate::Decompress failed (corrupt data or size mismatch)."));
        }
        return decompressed;
    }

    RHI::RhiExpected<std::vector<std::byte>> compress_gdeflate(std::span<const std::byte> data, u32 level) {
        ZoneScopedN("Core::compress_gdeflate");
        if (data.empty()) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::InvalidArgument, "compress_gdeflate: empty input."));
        }

        const usize bound = GDeflate::CompressBound(data.size());
        std::vector<std::byte> output(bound);
        size_t output_size = bound;
        const bool ok = GDeflate::Compress(
            reinterpret_cast<uint8_t *>(output.data()), &output_size,
            reinterpret_cast<const uint8_t *>(data.data()), data.size(),
            level, /*flags=*/0);
        if (!ok) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                             "compress_gdeflate: GDeflate::Compress failed."));
        }
        output.resize(output_size);
        return output;
    }

} // namespace SFT::Core
