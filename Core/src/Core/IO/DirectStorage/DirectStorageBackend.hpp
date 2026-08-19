#pragma once

#include <RHI/RHI.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace SFT::Core {


    /// Returns the current or globally available direct storage available value.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool direct_storage_available() noexcept;


    /// Reads file direct storage from the associated source.
    ///
    /// @param path Filesystem path identifying the target resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `RhiErrorCode::Unsupported`, `RhiErrorCode::OperationFailed`.
    [[nodiscard]] RHI::RhiExpected<std::vector<std::byte>> read_file_direct_storage(
        const std::filesystem::path &path);

} // namespace SFT::Core
