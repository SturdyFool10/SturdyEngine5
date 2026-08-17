#include "Decompression.hpp"

#include <Foundation/src/Foundation.hpp>









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
                                 1);
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
            level,           0);
        if (!ok) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                             "compress_gdeflate: GDeflate::Compress failed."));
        }
        output.resize(output_size);
        return output;
    }

} // namespace SFT::Core
