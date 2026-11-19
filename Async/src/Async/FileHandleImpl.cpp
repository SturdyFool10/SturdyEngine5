#include <Async/FileHandle.hpp>
#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Size.hpp>
#include <Async/NativeFileIo.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <new>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using std::expected;
using std::unexpected;
using SFT::Async::Literals::operator""_MiB;

namespace SFT::Async::Detail {

    /// The descriptor lives inside the mutex, so it cannot be touched (or closed) without holding the lock.
    struct FileState {
        Mutex<int> fd{-1};
        bool readable = false;
        bool writable = false;
        bool append = false;
    };

    namespace {

        constexpr std::size_t io_chunk = 1_MiB;

        IoError errno_error(const char *verb, int error) {
            IoErrorCode code = IoErrorCode::Unknown;
            switch (error) {
                case EACCES:
                case EPERM:
                case EROFS:
                    code = IoErrorCode::PermissionDenied;
                    break;
                case ENOENT:
                    code = IoErrorCode::NotFound;
                    break;
                case EEXIST:
                    code = IoErrorCode::AlreadyExists;
                    break;
                case EINVAL:
                case EBADF:
                case EISDIR:
                    code = IoErrorCode::InvalidArgument;
                    break;
                default:
                    break;
            }
            return IoError{code, std::string(verb) + " failed: " + std::strerror(error)};
        }

        IoError closed_error() { return IoError{IoErrorCode::InvalidArgument, "operation on a closed File"}; }

#if defined(_WIN32)
        using Offset = std::int64_t;
        long long sys_read(int fd, void *buffer, std::size_t count) {
            return _read(fd, buffer, static_cast<unsigned>((std::min)(count, std::size_t{1} << 30)));
        }
        long long sys_write(int fd, const void *buffer, std::size_t count) {
            return _write(fd, buffer, static_cast<unsigned>((std::min)(count, std::size_t{1} << 30)));
        }
        Offset sys_seek(int fd, Offset offset, int whence) { return _lseeki64(fd, offset, whence); }
        // No native positional IO in the CRT: seek, transfer, restore. Safe because the state mutex is held.
        long long sys_pread(int fd, void *buffer, std::size_t count, Offset offset) {
            const Offset saved = _lseeki64(fd, 0, SEEK_CUR);
            if (saved < 0 || _lseeki64(fd, offset, SEEK_SET) < 0) {
                return -1;
            }
            const long long got = sys_read(fd, buffer, count);
            const int error = errno;
            (void)_lseeki64(fd, saved, SEEK_SET);
            errno = error;
            return got;
        }
        long long sys_pwrite(int fd, const void *buffer, std::size_t count, Offset offset) {
            const Offset saved = _lseeki64(fd, 0, SEEK_CUR);
            if (saved < 0 || _lseeki64(fd, offset, SEEK_SET) < 0) {
                return -1;
            }
            const long long put = sys_write(fd, buffer, count);
            const int error = errno;
            (void)_lseeki64(fd, saved, SEEK_SET);
            errno = error;
            return put;
        }
        int sys_sync(int fd, bool) { return _commit(fd); }
        int sys_truncate(int fd, std::uint64_t length) { return _chsize_s(fd, static_cast<std::int64_t>(length)) == 0 ? 0 : -1; }
        int sys_close(int fd) { return _close(fd); }
        bool sys_size(int fd, std::uint64_t &out) {
            struct _stat64 info{};
            if (_fstat64(fd, &info) != 0) {
                return false;
            }
            out = static_cast<std::uint64_t>(info.st_size);
            return true;
        }
#else
        using Offset = off_t;
        long long sys_read(int fd, void *buffer, std::size_t count) { return ::read(fd, buffer, count); }
        long long sys_write(int fd, const void *buffer, std::size_t count) { return ::write(fd, buffer, count); }
        Offset sys_seek(int fd, Offset offset, int whence) { return ::lseek(fd, offset, whence); }
        long long sys_pread(int fd, void *buffer, std::size_t count, Offset offset) { return ::pread(fd, buffer, count, offset); }
        long long sys_pwrite(int fd, const void *buffer, std::size_t count, Offset offset) { return ::pwrite(fd, buffer, count, offset); }
        int sys_sync(int fd, bool data_only) {
#if defined(__linux__)
            return data_only ? ::fdatasync(fd) : ::fsync(fd);
#else
            (void)data_only;
            return ::fsync(fd);
#endif
        }
        int sys_truncate(int fd, std::uint64_t length) { return ::ftruncate(fd, static_cast<off_t>(length)); }
        int sys_close(int fd) { return ::close(fd); }
        bool sys_size(int fd, std::uint64_t &out) {
            struct stat info{};
            if (::fstat(fd, &info) != 0) {
                return false;
            }
            out = static_cast<std::uint64_t>(info.st_size);
            return true;
        }
#endif

        /// Reads until `max_bytes` or EOF. `offset == nullptr` uses and advances the cursor.
        /// Memory grows with the bytes that actually arrive, never with the requested ceiling.
        expected<std::vector<std::byte>, IoError> read_locked(int fd, std::size_t max_bytes, const std::uint64_t *offset,
                                                              CancellationToken &cancellation) {
            std::vector<std::byte> out;
            std::uint64_t position = offset != nullptr ? *offset : 0;
            try {
                while (out.size() < max_bytes) {
                    cancellation.throw_if_cancelled();
                    const std::size_t want = (std::min)(max_bytes - out.size(), io_chunk);
                    const std::size_t at = out.size();
                    out.resize(at + want);
                    long long got = 0;
                    do {
                        got = offset != nullptr ? sys_pread(fd, out.data() + at, want, static_cast<Offset>(position + at))
                                                : sys_read(fd, out.data() + at, want);
                    } while (got < 0 && errno == EINTR);
                    if (got < 0) {
                        return unexpected(errno_error("read", errno));
                    }
                    out.resize(at + static_cast<std::size_t>(got));
                    if (got == 0) {
                        break;
                    }
                }
            } catch (const std::bad_alloc &) {
                return unexpected(IoError{IoErrorCode::Unknown, "Failed to allocate read buffer"});
            }
            return out;
        }

    } // namespace

    expected<File, IoError> file_open_blocking(const std::string &path, OpenMode mode) {
        const bool create_new = has_mode(mode, OpenMode::CreateNew);
        const bool append = has_mode(mode, OpenMode::Append);
        const bool readable = has_mode(mode, OpenMode::Read);
        const bool writable = has_mode(mode, OpenMode::Write) || append || create_new;
        if (!readable && !writable) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "open() needs Read, Write or Append"});
        }
        if ((has_mode(mode, OpenMode::Create) || has_mode(mode, OpenMode::Truncate)) && !writable) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "Create/Truncate require write access"});
        }
        if (has_mode(mode, OpenMode::Truncate) && append && !has_mode(mode, OpenMode::Write)) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "Truncate cannot be combined with Append alone"});
        }

#if defined(_WIN32)
        int flags = _O_BINARY | _O_NOINHERIT;
        flags |= (readable && writable) ? _O_RDWR : (writable ? _O_WRONLY : _O_RDONLY);
        if (append) flags |= _O_APPEND;
        if (create_new) flags |= _O_CREAT | _O_EXCL;
        else if (has_mode(mode, OpenMode::Create)) flags |= _O_CREAT;
        if (has_mode(mode, OpenMode::Truncate)) flags |= _O_TRUNC;
        int fd = -1;
        const int status = _wsopen_s(&fd, std::filesystem::path(path).c_str(), flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
        if (status != 0) {
            return unexpected(errno_error("open", status));
        }
#else
        int flags = O_CLOEXEC;
        flags |= (readable && writable) ? O_RDWR : (writable ? O_WRONLY : O_RDONLY);
        if (append) flags |= O_APPEND;
        if (create_new) flags |= O_CREAT | O_EXCL;
        else if (has_mode(mode, OpenMode::Create)) flags |= O_CREAT;
        if (has_mode(mode, OpenMode::Truncate)) flags |= O_TRUNC;
        int fd = -1;
        do {
            fd = ::open(path.c_str(), flags, 0666);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0) {
            return unexpected(errno_error("open", errno));
        }
#endif
        try {
            auto state = std::make_shared<FileState>();
            *state->fd.lock() = fd;
            state->readable = readable;
            state->writable = writable;
            state->append = append;
            return File(std::move(state));
        } catch (...) {
            sys_close(fd);
            return unexpected(IoError{IoErrorCode::Unknown, "Failed to allocate File state"});
        }
    }

    expected<std::vector<std::byte>, IoError> file_read_blocking(const std::shared_ptr<FileState> &state, std::size_t max_bytes,
                                                                 const std::uint64_t *offset, CancellationToken cancellation) {
        if (!state) return unexpected(closed_error());
        auto fd = state->fd.lock();
        if (*fd < 0) return unexpected(closed_error());
        if (!state->readable) return unexpected(IoError{IoErrorCode::InvalidArgument, "File was not opened for reading"});
        return read_locked(*fd, max_bytes, offset, cancellation);
    }

    expected<std::vector<std::byte>, IoError> file_read_to_end_blocking(const std::shared_ptr<FileState> &state, std::size_t max_bytes,
                                                                        CancellationToken cancellation) {
        if (!state) return unexpected(closed_error());
        auto fd = state->fd.lock();
        if (*fd < 0) return unexpected(closed_error());
        if (!state->readable) return unexpected(IoError{IoErrorCode::InvalidArgument, "File was not opened for reading"});
        // Ask for one byte more than allowed: getting it proves the limit was exceeded.
        const std::size_t probe = max_bytes == (std::numeric_limits<std::size_t>::max)() ? max_bytes : max_bytes + 1;
        auto data = read_locked(*fd, probe, nullptr, cancellation);
        if (data && data->size() > max_bytes) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "file exceeds the read_to_end limit"});
        }
        return data;
    }

    expected<std::size_t, IoError> file_write_blocking(const std::shared_ptr<FileState> &state, std::span<const std::byte> data,
                                                       const std::uint64_t *offset, CancellationToken cancellation) {
        if (!state) return unexpected(closed_error());
        auto fd = state->fd.lock();
        if (*fd < 0) return unexpected(closed_error());
        if (!state->writable) return unexpected(IoError{IoErrorCode::InvalidArgument, "File was not opened for writing"});
        if (offset != nullptr && state->append) {
            return unexpected(IoError{IoErrorCode::InvalidArgument, "write_at is not valid on an Append file"});
        }
        std::size_t written = 0;
        while (written < data.size()) {
            cancellation.throw_if_cancelled();
            const std::size_t want = (std::min)(data.size() - written, io_chunk);
            long long put = 0;
            do {
                put = offset != nullptr ? sys_pwrite(*fd, data.data() + written, want, static_cast<Offset>(*offset + written))
                                        : sys_write(*fd, data.data() + written, want);
            } while (put < 0 && errno == EINTR);
            if (put < 0) {
                return unexpected(errno_error("write", errno));
            }
            if (put == 0) {
                return unexpected(IoError{IoErrorCode::Unknown, "write made no progress"});
            }
            written += static_cast<std::size_t>(put);
        }
        return written;
    }

    expected<std::uint64_t, IoError> file_seek_blocking(const std::shared_ptr<FileState> &state, std::int64_t offset, SeekOrigin origin) {
        if (!state) return unexpected(closed_error());
        auto fd = state->fd.lock();
        if (*fd < 0) return unexpected(closed_error());
        const int whence = origin == SeekOrigin::Begin ? SEEK_SET : origin == SeekOrigin::Current ? SEEK_CUR : SEEK_END;
        const auto position = sys_seek(*fd, static_cast<Offset>(offset), whence);
        if (position < 0) {
            return unexpected(errno_error("seek", errno));
        }
        return static_cast<std::uint64_t>(position);
    }

    expected<std::uint64_t, IoError> file_handle_size_blocking(const std::shared_ptr<FileState> &state) {
        if (!state) return unexpected(closed_error());
        auto fd = state->fd.lock();
        if (*fd < 0) return unexpected(closed_error());
        std::uint64_t size = 0;
        if (!sys_size(*fd, size)) {
            return unexpected(errno_error("fstat", errno));
        }
        return size;
    }

    expected<void, IoError> file_set_len_blocking(const std::shared_ptr<FileState> &state, std::uint64_t length) {
        if (!state) return unexpected(closed_error());
        auto fd = state->fd.lock();
        if (*fd < 0) return unexpected(closed_error());
        if (!state->writable) return unexpected(IoError{IoErrorCode::InvalidArgument, "File was not opened for writing"});
        if (sys_truncate(*fd, length) != 0) {
            return unexpected(errno_error("truncate", errno));
        }
        return {};
    }

    expected<void, IoError> file_sync_blocking(const std::shared_ptr<FileState> &state, bool data_only) {
        if (!state) return unexpected(closed_error());
        auto fd = state->fd.lock();
        if (*fd < 0) return unexpected(closed_error());
        if (sys_sync(*fd, data_only) != 0) {
            return unexpected(errno_error("fsync", errno));
        }
        return {};
    }

    // ---- native async path (positional operations; no IO-lane thread while the platform does the IO) -----------------
    // io_uring on Linux, overlapped IO + a completion port on Windows, Grand Central Dispatch on macOS: see NativeFileIo.hpp.
#if !defined(STURDY_PLATFORM_WEB)
    namespace {
        /// A big read is cut into chunks of this size, all submitted at once, so the device sees a deep queue and the copies run on
        /// several cores. Memory is still bounded by the bytes that can actually arrive (the file's size, not the requested ceiling).
        constexpr std::size_t native_read_chunk = 2 * 1024 * 1024;
        constexpr std::size_t native_max_step = std::size_t{1} << 30;

        /// A private handle for one operation, so `close()` on the File cannot pull it out from under the platform; also reports
        /// the file's current size when asked (reads are clamped to it).
        nativeio::Handle duplicate_native(const std::shared_ptr<FileState> &state, bool need_write, std::uint64_t *size = nullptr) {
            if (!state) return nativeio::kInvalidHandle;
            auto fd = state->fd.lock();
            if (*fd < 0 || (need_write ? !state->writable || state->append : !state->readable)) return nativeio::kInvalidHandle;
            if (size != nullptr && !sys_size(*fd, *size)) return nativeio::kInvalidHandle;
            return nativeio::duplicate(*fd, need_write);
        }

        struct NativeReadOp {
            struct Chunk {
                std::size_t begin = 0;
                std::size_t length = 0;
                std::size_t filled = 0;
            };

            nativeio::Handle handle = nativeio::kInvalidHandle;
            std::vector<std::byte> buffer;
            std::vector<Chunk> chunks;
            std::uint64_t offset = 0;
            std::atomic<std::size_t> pending{0};
            std::atomic<int> error{0}; ///< first errno seen; 0 while all is well
            Promise<expected<std::vector<std::byte>, IoError>> promise;

            void chunk_done() {
                if (pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
                nativeio::close(handle);
                if (const int e = error.load(std::memory_order_acquire); e != 0) {
                    (void)promise.set_value(unexpected(errno_error("read", e)));
                    return;
                }
                // The result is the contiguous prefix that arrived: a chunk that came back short ended the file.
                std::size_t total = 0;
                for (const Chunk &chunk : chunks) {
                    total += chunk.filled;
                    if (chunk.filled < chunk.length) break;
                }
                buffer.resize(total);
                (void)promise.set_value(std::move(buffer));
            }
        };

        void step_read(const std::shared_ptr<NativeReadOp> &op, std::size_t index) {
            NativeReadOp::Chunk &chunk = op->chunks[index];
            const std::size_t want = (std::min)(chunk.length - chunk.filled, native_max_step);
            nativeio::read(op->handle, op->buffer.data() + chunk.begin + chunk.filled, want, op->offset + chunk.begin + chunk.filled,
                           [op, index](long long result) {
                               NativeReadOp::Chunk &c = op->chunks[index];
                               if (result == -EINTR) return step_read(op, index);
                               if (result < 0) {
                                   int none = 0;
                                   op->error.compare_exchange_strong(none, static_cast<int>(-result));
                                   return op->chunk_done();
                               }
                               c.filled += static_cast<std::size_t>(result);
                               if (result == 0 || c.filled == c.length) return op->chunk_done(); // 0 = end of file
                               step_read(op, index);
                           });
        }

        struct NativeWriteOp {
            nativeio::Handle handle = nativeio::kInvalidHandle;
            std::vector<std::byte> data;
            std::size_t written = 0;
            std::uint64_t offset = 0;
            Promise<expected<std::size_t, IoError>> promise;

            void finish(expected<std::size_t, IoError> result) {
                nativeio::close(handle);
                (void)promise.set_value(std::move(result));
            }
        };

        void step_write(const std::shared_ptr<NativeWriteOp> &op) {
            const std::size_t want = (std::min)(op->data.size() - op->written, native_max_step);
            nativeio::write(op->handle, op->data.data() + op->written, want, op->offset + op->written, [op](long long result) {
                if (result == -EINTR) return step_write(op);
                if (result < 0) return op->finish(unexpected(errno_error("write", static_cast<int>(-result))));
                if (result == 0) return op->finish(unexpected(IoError{IoErrorCode::Unknown, "write made no progress"}));
                op->written += static_cast<std::size_t>(result);
                if (op->written == op->data.size()) return op->finish(op->written);
                step_write(op);
            });
        }
    } // namespace

    bool file_uring_available() noexcept { return nativeio::available(); }

    TaskHandle<expected<std::vector<std::byte>, IoError>> file_read_at_uring(const std::shared_ptr<FileState> &state, std::uint64_t offset,
                                                                             std::size_t max_bytes, const CancellationToken &cancellation) {
        if (max_bytes == 0 || cancellation.is_cancelled() || !nativeio::available()) return {};
        std::uint64_t size = 0;
        const nativeio::Handle handle = duplicate_native(state, false, &size);
        if (handle == nativeio::kInvalidHandle) return {};
        // Regular files report a size; anything that reports none (a pipe, a device) takes the lane, which reads until it ends.
        const std::uint64_t available_bytes = size > offset ? size - offset : 0;
        const std::size_t want = static_cast<std::size_t>((std::min<std::uint64_t>)(max_bytes, available_bytes));
        if (want == 0) {
            nativeio::close(handle);
            return {};
        }
        auto op = std::make_shared<NativeReadOp>();
        op->handle = handle;
        op->offset = offset;
        try {
            op->buffer.resize(want);
        } catch (const std::bad_alloc &) {
            nativeio::close(handle);
            return {};
        }
        for (std::size_t begin = 0; begin < want; begin += native_read_chunk) {
            op->chunks.push_back({begin, (std::min)(native_read_chunk, want - begin), 0});
        }
        op->pending.store(op->chunks.size(), std::memory_order_release);
        auto result = op->promise.handle();
        for (std::size_t i = 0; i < op->chunks.size(); ++i) step_read(op, i);
        return result;
    }

    TaskHandle<expected<std::size_t, IoError>> file_write_at_uring(const std::shared_ptr<FileState> &state, std::uint64_t offset,
                                                                   std::vector<std::byte> &data, const CancellationToken &cancellation) {
        if (data.empty() || cancellation.is_cancelled() || !nativeio::available()) return {};
        const nativeio::Handle handle = duplicate_native(state, true);
        if (handle == nativeio::kInvalidHandle) return {};
        auto op = std::make_shared<NativeWriteOp>();
        op->handle = handle;
        op->data = std::move(data);
        op->offset = offset;
        auto result = op->promise.handle();
        step_write(op);
        return result;
    }

    TaskHandle<expected<void, IoError>> file_sync_uring(const std::shared_ptr<FileState> &state, bool data_only) {
        if (!nativeio::available() || !state) return {};
        nativeio::Handle handle;
        {
            auto locked = state->fd.lock();
            if (*locked < 0) return {};
            handle = nativeio::duplicate(*locked, state->writable);
        }
        if (handle == nativeio::kInvalidHandle) return {};
        Promise<expected<void, IoError>> promise;
        auto result = promise.handle();
        nativeio::fsync(handle, data_only, [handle, promise](long long status) mutable {
            nativeio::close(handle);
            if (status < 0) (void)promise.set_value(unexpected(errno_error("fsync", static_cast<int>(-status))));
            else (void)promise.set_value(expected<void, IoError>{});
        });
        return result;
    }
#else
    bool file_uring_available() noexcept { return false; }
    TaskHandle<expected<std::vector<std::byte>, IoError>> file_read_at_uring(const std::shared_ptr<FileState> &, std::uint64_t, std::size_t, const CancellationToken &) { return {}; }
    TaskHandle<expected<std::size_t, IoError>> file_write_at_uring(const std::shared_ptr<FileState> &, std::uint64_t, std::vector<std::byte> &, const CancellationToken &) { return {}; }
    TaskHandle<expected<void, IoError>> file_sync_uring(const std::shared_ptr<FileState> &, bool) { return {}; }
#endif

    bool file_is_open(const std::shared_ptr<FileState> &state) noexcept {
        if (!state) return false;
        return *state->fd.lock() >= 0;
    }

    void file_close(const std::shared_ptr<FileState> &state) noexcept {
        if (!state) return;
        auto fd = state->fd.lock();
        if (*fd >= 0) {
            (void)sys_close(*fd);
            *fd = -1;
        }
    }

} // namespace SFT::Async::Detail
