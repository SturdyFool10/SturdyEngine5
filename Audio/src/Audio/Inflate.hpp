#pragma once

#include <Foundation/Foundation.hpp>

#include <expected>
#include <span>
#include <vector>

namespace SFT::Audio {

    /// DEFLATE decompression (RFC 1951), optionally behind a zlib header (RFC 1950; the checksum is not verified). Bounds-checked on
    /// every read, so damaged data fails with an error instead of reading past the input. `expected_size`, when known, reserves the
    /// output and stops runaway streams.
    [[nodiscard]] std::expected<std::vector<std::byte>, UString> inflate(std::span<const std::byte> compressed, bool zlib_header, usize expected_size = 0);

} // namespace SFT::Audio
