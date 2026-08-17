#include <Foundation/src/Foundation.hpp>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <vector>
#include <Async/src/File.hpp>

using std::expected;
using std::ifstream;
using std::ofstream;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

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

    /// Reads file blocking from the associated source.
    ///
    /// @param path Filesystem path identifying the target resource.
    ///
    /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
    /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
    /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::Unknown`.
    expected<vector<std::byte>, IoError> read_file_blocking(const string &path) {
        errno = 0;
        ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return unexpected(io_error_from_errno(path, "open"));
        }

        const std::streamoff size = file.tellg();
        if (size < 0) {
            return unexpected(IoError{IoErrorCode::Unknown, "Failed to determine size of '" + path + "'"});
        }
        file.seekg(0, std::ios::beg);

        vector<std::byte> data(static_cast<usize>(size));
        if (size > 0 && !file.read(reinterpret_cast<char *>(data.data()), size)) {
            return unexpected(IoError{IoErrorCode::Unknown, "Failed to read contents of '" + path + "'"});
        }
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
    expected<void, IoError> write_file_blocking(const string &path, span<const std::byte> data, bool append) {
        errno = 0;
        const auto mode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
        ofstream file(path, mode);
        if (!file.is_open()) {
            return unexpected(io_error_from_errno(path, "open"));
        }

        if (!data.empty() &&
            !file.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()))) {
            return unexpected(IoError{IoErrorCode::Unknown, "Failed to write contents of '" + path + "'"});
        }
        return {};
    }

} // namespace SFT::Async::Detail
