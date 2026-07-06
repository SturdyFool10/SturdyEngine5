module;

#pragma region Imports
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <vector>
#pragma endregion

module Sturdy.Async;

import :File;
import :IoError;
import Sturdy.Foundation;

using std::expected;
using std::ifstream;
using std::ofstream;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Async::Detail {

    namespace {

        // Best-effort: most standard library implementations set `errno` from the underlying
        // POSIX/CRT call on an fstream failure, but it isn't mandated by the standard, so this is a
        // classification hint, not a guarantee — callers that need certainty should stat the path
        // themselves first.
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
