#pragma once

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Async/IoError.hpp>
#include <Async/Runtime.hpp>

namespace SFT::Async {

    namespace Detail {
        /// Reads file blocking from the associated source.
        ///
        /// @param path Filesystem path identifying the target resource.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::Unknown`.
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> read_file_blocking(const std::string &path);
        /// Writes file blocking to the associated destination.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param data Data consumed or referenced by the operation.
        /// @param append `append` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        [[nodiscard]] std::expected<void, IoError> write_file_blocking(const std::string &path, std::span<const std::byte> data, bool append);
    } // namespace Detail

    /// Reads file from the associated source.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto read_file(std::string path) {
        return Rt::spawn([path = std::move(path)]() mutable {
            return Detail::read_file_blocking(path);
        });
    }

    template <AsyncRuntime Rt = DefaultRuntime>
    [[nodiscard]] auto write_file(std::string path, std::vector<std::byte> data, bool append = false) {
        return Rt::spawn([path = std::move(path), data = std::move(data), append]() mutable {
            return Detail::write_file_blocking(path, data, append);
        });
    }

} // namespace SFT::Async
