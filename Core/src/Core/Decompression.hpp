#pragma once

#include <Foundation/src/Foundation.hpp>

#include <RHI/RHI.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace SFT::Core {


    /// Decompresses gdeflate into its uncompressed representation.
    ///
    /// @param compressed `compressed` value used by the operation.
    /// @param decompressed_size Requested or available size for the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::InvalidArgument`, `RhiErrorCode::OperationFailed`.
    [[nodiscard]] RHI::RhiExpected<std::vector<std::byte>> decompress_gdeflate(
        std::span<const std::byte> compressed, u64 decompressed_size);


    /// Compresses gdeflate into the requested representation.
    ///
    /// @param data Data consumed or referenced by the operation.
    /// @param level `level` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    [[nodiscard]] RHI::RhiExpected<std::vector<std::byte>> compress_gdeflate(
        std::span<const std::byte> data, u32 level = 9);

} // namespace SFT::Core
