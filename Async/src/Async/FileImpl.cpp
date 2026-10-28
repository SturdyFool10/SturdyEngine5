#include <Foundation/Foundation.hpp>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <fstream>
#include <filesystem>
#include <ios>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include <Async/File.hpp>
#include <Async/Size.hpp>

using std::expected;
using std::ifstream;
using std::ofstream;
using std::span;
using std::string;
using std::unexpected;
using std::vector;
using SFT::Async::Literals::operator""_MiB;

namespace SFT::Async::Detail {

    namespace {


        /// Performs the I/O error from errno operation for `Detail` using the supplied arguments.
        ///
        /// @param path Filesystem path identifying the target resource.
        /// @param verb `verb` value used by the operation.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function does not throw exceptions.
        IoError io_error_from_errno(const string &path, const char *verb) noexcept {
            IoErrorCode code = IoErrorCode::Unknown;
            switch (errno) {
                case EACCES:
                case EPERM:
                    code = IoErrorCode::PermissionDenied;
                    break;
                case ENOENT:
                    code = IoErrorCode::NotFound;
                    break;
                case EEXIST:
                    code = IoErrorCode::AlreadyExists;
                    break;
                default:
                    break;
            }
            string message = string(verb) + " failed for '" + path + "'";
            if (const char *errno_message = std::strerror(errno)) {
                message += ": ";
                message += errno_message;
            }
            return IoError{code, std::move(message)};
        }

    } // namespace

    namespace {
        IoError io_error_from_code(const string &path, const char *verb, const std::error_code &error) {
            IoErrorCode code = IoErrorCode::Unknown;
            if (error == std::errc::permission_denied) code = IoErrorCode::PermissionDenied;
            else if (error == std::errc::no_such_file_or_directory) code = IoErrorCode::NotFound;
            else if (error == std::errc::file_exists) code = IoErrorCode::AlreadyExists;
            else if (error == std::errc::invalid_argument || error == std::errc::not_a_directory) code = IoErrorCode::InvalidArgument;
            return IoError{code, string(verb) + " failed for '" + path + "': " + error.message()};
        }
    }

} // namespace SFT::Async::Detail

namespace SFT::Async {

    DirectoryIterator::DirectoryIterator(string path, bool recursive) : recursive_(recursive) {
        std::error_code error;
        if (recursive_) {
            recursive_iterator_ = std::filesystem::recursive_directory_iterator(path, error);
        } else {
            iterator_ = std::filesystem::directory_iterator(path, error);
        }
        if (error) throw std::filesystem::filesystem_error("list directory", path, error);
        if (has_next()) current_ = recursive_ ? recursive_iterator_->path().string() : iterator_->path().string();
    }

    bool DirectoryIterator::has_next() const noexcept {
        return !error_ && (recursive_ ? recursive_iterator_ != recursive_end_ : iterator_ != end_);
    }

    const string &DirectoryIterator::value() const {
        if (!has_next()) throw std::logic_error("directory iterator has no current entry");
        return current_;
    }

    void DirectoryIterator::advance() noexcept {
        if (!has_next()) return;
        std::error_code error;
        if (recursive_) recursive_iterator_.increment(error);
        else iterator_.increment(error);
        if (error) {
            error_ = IoError{IoErrorCode::Unknown, "advance directory iterator failed: " + error.message()};
            iterator_ = end_;
            current_.clear();
            return;
        }
        if (has_next()) current_ = recursive_ ? recursive_iterator_->path().string() : iterator_->path().string();
        else current_.clear();
    }

    expected<std::optional<string>, IoError> DirectoryIterator::next() {
        if (error_) return unexpected(*error_);
        if (!has_next()) return std::optional<string>{};
        string result = current_;
        advance();
        if (error_) return unexpected(*error_);
        return std::optional<string>{std::move(result)};
    }

    DirectoryIterator &DirectoryIterator::operator++() {
        advance();
        if (error_) throw std::filesystem::filesystem_error("advance directory iterator", current_,
                                                             std::make_error_code(std::errc::io_error));
        return *this;
    }

} // namespace SFT::Async

namespace SFT::Async::Detail {

    expected<vector<string>, IoError> list_directory_blocking(
        const string &path, usize max_entries, bool recursive, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        std::error_code error;
        vector<string> entries;
        const auto add_entry = [&](const auto &entry) -> bool {
            if (entries.size() == max_entries) return false;
            entries.push_back(entry->path().string());
            return true;
        };
        if (recursive) {
            std::filesystem::recursive_directory_iterator iterator(path, error);
            if (error) return unexpected(io_error_from_code(path, "list directory", error));
            const std::filesystem::recursive_directory_iterator end;
            for (; iterator != end; iterator.increment(error)) {
                cancellation.throw_if_cancelled();
                if (error) return unexpected(io_error_from_code(path, "list directory", error));
                if (!add_entry(iterator)) return unexpected(IoError{IoErrorCode::InvalidArgument, "Directory entry limit exceeded: '" + path + "'"});
            }
        } else {
            std::filesystem::directory_iterator iterator(path, error);
            if (error) return unexpected(io_error_from_code(path, "list directory", error));
            const std::filesystem::directory_iterator end;
            for (; iterator != end; iterator.increment(error)) {
                cancellation.throw_if_cancelled();
                if (error) return unexpected(io_error_from_code(path, "list directory", error));
                if (!add_entry(iterator)) return unexpected(IoError{IoErrorCode::InvalidArgument, "Directory entry limit exceeded: '" + path + "'"});
            }
        }
        if (error) return unexpected(io_error_from_code(path, "list directory", error));
        cancellation.throw_if_cancelled();
        return entries;
    }

    expected<void, IoError> rename_file_blocking(const string &source, const string &destination, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        std::error_code error;
        std::filesystem::rename(source, destination, error);
        if (error) return unexpected(io_error_from_code(source + "' to '" + destination, "rename", error));
        cancellation.throw_if_cancelled();
        return {};
    }

    expected<bool, IoError> remove_directory_blocking(const string &path, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        std::error_code error;
        const bool removed = std::filesystem::remove(path, error);
        if (error) return unexpected(io_error_from_code(path, "remove directory", error));
        cancellation.throw_if_cancelled();
        return removed;
    }

    expected<bool, IoError> file_exists_blocking(const string &path, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) return unexpected(io_error_from_code(path, "exists", error));
        cancellation.throw_if_cancelled();
        return exists;
    }

    expected<void, IoError> create_directories_blocking(const string &path, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        std::error_code error;
        (void)std::filesystem::create_directories(path, error);
        if (error) return unexpected(io_error_from_code(path, "create directories", error));
        cancellation.throw_if_cancelled();
        return {};
    }

    expected<bool, IoError> remove_file_blocking(const string &path, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        std::error_code error;
        const bool removed = std::filesystem::remove(path, error);
        if (error) return unexpected(io_error_from_code(path, "remove", error));
        cancellation.throw_if_cancelled();
        return removed;
    }

    expected<void, IoError> copy_file_blocking(const string &source, const string &destination, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        std::error_code error;
        (void)std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
        if (error) return unexpected(io_error_from_code(source + "' to '" + destination, "copy", error));
        cancellation.throw_if_cancelled();
        return {};
    }

    expected<vector<std::byte>, IoError> read_file_range_blocking(
        const string &path, std::uint64_t offset, usize byte_count, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "File range offset is not representable: '" + path + "'"});
        }

        errno = 0;
        ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return unexpected(io_error_from_errno(path, "open"));
        }
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file) {
            return unexpected(IoError{IoErrorCode::Unknown, "Failed to seek in '" + path + "'"});
        }

        vector<std::byte> data(byte_count);
        constexpr usize chunk_size = 1_MiB;
        usize read_total = 0;
        while (read_total < byte_count) {
            cancellation.throw_if_cancelled();
            const usize chunk = std::min(chunk_size, byte_count - read_total);
            file.read(reinterpret_cast<char *>(data.data() + read_total), static_cast<std::streamsize>(chunk));
            const std::streamsize count = file.gcount();
            if (count > 0) {
                read_total += static_cast<usize>(count);
            }
            if (file.bad()) {
                return unexpected(IoError{IoErrorCode::Unknown, "Failed to read range of '" + path + "'"});
            }
            if (file.eof()) {
                break;
            }
        }
        data.resize(read_total);
        cancellation.throw_if_cancelled();
        return data;
    }

    expected<usize, IoError> file_size_blocking(const string &path, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        errno = 0;
        ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return unexpected(io_error_from_errno(path, "open"));
        }
        const std::streamoff size = file.tellg();
        if (size < 0) {
            return unexpected(IoError{IoErrorCode::Unknown, "Failed to determine size of '" + path + "'"});
        }
        if (static_cast<std::uintmax_t>(size) > (std::numeric_limits<usize>::max)()) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "File size exceeds addressable memory: '" + path + "'"});
        }
        cancellation.throw_if_cancelled();
        return static_cast<usize>(size);
    }

    /// Reads file blocking from the associated source.
    ///
    /// @param path Filesystem path identifying the target resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::Unknown`.
    expected<vector<std::byte>, IoError> read_file_blocking(
            const string &path, usize max_bytes, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        errno = 0;
        ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return unexpected(io_error_from_errno(path, "open"));
        }

        const std::streamoff size = file.tellg();
        if (size < 0) {
            return unexpected(IoError{IoErrorCode::Unknown, "Failed to determine size of '" + path + "'"});
        }
        if (static_cast<std::uintmax_t>(size) > max_bytes ||
            static_cast<std::uintmax_t>(size) > (std::numeric_limits<usize>::max)()) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "File exceeds configured read size limit: '" + path + "'"});
        }
        file.seekg(0, std::ios::beg);

        vector<std::byte> data(static_cast<usize>(size));
        constexpr usize chunk_size = 1_MiB;
        usize offset = 0;
        const usize total_size = data.size();
        while (offset < total_size) {
            cancellation.throw_if_cancelled();
            const usize chunk = std::min(chunk_size, total_size - offset);
            if (!file.read(reinterpret_cast<char *>(data.data() + offset), static_cast<std::streamsize>(chunk))) {
                return unexpected(IoError{IoErrorCode::Unknown, "Failed to read contents of '" + path + "'"});
            }
            offset += chunk;
        }
        cancellation.throw_if_cancelled();
        return data;
    }

    /// Writes file blocking to the associated destination.
    ///
    /// @param path Filesystem path identifying the target resource.
    /// @param data Data consumed or referenced by the operation.
    /// @param append `append` value used by the operation.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::Unknown`.
    expected<void, IoError> write_file_blocking(
        const string &path, span<const std::byte> data, bool append, CancellationToken cancellation) {
        cancellation.throw_if_cancelled();
        errno = 0;
        const auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
        ofstream file(path, mode);
        if (!file.is_open()) {
            return unexpected(io_error_from_errno(path, "open"));
        }

        constexpr usize chunk_size = 1_MiB;
        usize offset = 0;
        while (offset < data.size()) {
            cancellation.throw_if_cancelled();
            const usize chunk = std::min(chunk_size, data.size() - offset);
            if (!file.write(reinterpret_cast<const char *>(data.data() + offset), static_cast<std::streamsize>(chunk))) {
                return unexpected(IoError{IoErrorCode::Unknown, "Failed to write contents of '" + path + "'"});
            }
            offset += chunk;
        }
        cancellation.throw_if_cancelled();
        return {};
    }

} // namespace SFT::Async::Detail
