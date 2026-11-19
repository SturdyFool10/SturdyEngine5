#include <Async/NativeFileIo.hpp>

#include <Async/Uring.hpp>

#include <cerrno>
#include <memory>
#include <utility>

#if defined(__linux__) && !defined(STURDY_PLATFORM_WEB)
// ---- Linux: io_uring ------------------------------------------------------------------------------------------------------------

#include <unistd.h>

namespace SFT::Async::Detail::nativeio {

    bool available() noexcept { return uring::available(); }
    Handle duplicate(int fd, bool) noexcept {
        const int copy = ::dup(fd);
        return copy < 0 ? kInvalidHandle : static_cast<Handle>(copy);
    }
    void close(Handle handle) noexcept { ::close(static_cast<int>(handle)); }
    void read(Handle handle, void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        uring::read(static_cast<int>(handle), buffer, size, offset, std::move(done));
    }
    void write(Handle handle, const void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        uring::write(static_cast<int>(handle), buffer, size, offset, std::move(done));
    }
    void fsync(Handle handle, bool data_only, Completion done) { uring::fsync(static_cast<int>(handle), data_only, std::move(done)); }

} // namespace SFT::Async::Detail::nativeio

#elif defined(_WIN32)
// ---- Windows: overlapped IO + I/O completion port ----------------------------------------------------------------------------

#if !defined(NOMINMAX)
    #define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <io.h>

#include <memory>
#include <thread>

namespace SFT::Async::Detail::nativeio {

    namespace {

        /// Win32 error -> the errno-style code the file layer already knows how to describe.
        [[nodiscard]] int to_errno(DWORD error) noexcept {
            switch (error) {
                case ERROR_ACCESS_DENIED:
                case ERROR_SHARING_VIOLATION: return EACCES;
                case ERROR_FILE_NOT_FOUND:
                case ERROR_PATH_NOT_FOUND: return ENOENT;
                case ERROR_INVALID_HANDLE:
                case ERROR_INVALID_PARAMETER: return EINVAL;
                case ERROR_DISK_FULL:
                case ERROR_HANDLE_DISK_FULL: return ENOSPC;
                default: return EIO;
            }
        }

        /// One request in flight. The OVERLAPPED is first so a completion packet's pointer is the Op.
        struct Op {
            OVERLAPPED overlapped{};
            Completion done;
        };

        class Port {
          public:
            Port() {
                port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
                if (port_ == nullptr) return;
                reaper_ = std::thread([this] { reap(); });
            }
            ~Port() {
                if (port_ == nullptr) return;
                PostQueuedCompletionStatus(port_, 0, kStopKey, nullptr);
                reaper_.join();
                CloseHandle(port_);
            }
            [[nodiscard]] bool ready() const noexcept { return port_ != nullptr; }
            [[nodiscard]] HANDLE handle() const noexcept { return port_; }

          private:
            static constexpr ULONG_PTR kStopKey = 1;

            void reap() {
                for (;;) {
                    DWORD transferred = 0;
                    ULONG_PTR key = 0;
                    OVERLAPPED *overlapped = nullptr;
                    const BOOL ok = GetQueuedCompletionStatus(port_, &transferred, &key, &overlapped, INFINITE);
                    if (overlapped == nullptr) {
                        if (key == kStopKey) return;
                        continue; // the port itself failed without a packet: nothing to complete
                    }
                    std::unique_ptr<Op> op(reinterpret_cast<Op *>(overlapped));
                    long long result = static_cast<long long>(transferred);
                    if (!ok) {
                        const DWORD error = GetLastError();
                        result = error == ERROR_HANDLE_EOF ? 0 : -static_cast<long long>(to_errno(error));
                    }
                    op->done(result);
                }
            }

            HANDLE port_ = nullptr;
            std::thread reaper_;
        };

        Port &port() {
            static Port instance;
            return instance;
        }

        void submit(Handle handle, void *buffer, std::size_t size, std::uint64_t offset, Completion done, bool is_write) {
            auto op = std::make_unique<Op>();
            op->done = std::move(done);
            op->overlapped.Offset = static_cast<DWORD>(offset & 0xffffffffu);
            op->overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
            const HANDLE file = reinterpret_cast<HANDLE>(handle);
            const DWORD length = static_cast<DWORD>(size);
            const BOOL started = is_write ? WriteFile(file, buffer, length, nullptr, &op->overlapped)
                                          : ReadFile(file, buffer, length, nullptr, &op->overlapped);
            if (!started && GetLastError() != ERROR_IO_PENDING) {
                const DWORD error = GetLastError();
                op->done(error == ERROR_HANDLE_EOF ? 0 : -static_cast<long long>(to_errno(error)));
                return;
            }
            op.release(); // owned by the port until its completion packet arrives (also queued when the call finished at once)
        }

    } // namespace

    bool available() noexcept { return port().ready(); }

    Handle duplicate(int fd, bool for_write) noexcept {
        if (!port().ready()) return kInvalidHandle;
        const intptr_t os_handle = _get_osfhandle(fd);
        if (os_handle == -1) return kInvalidHandle;
        // A new handle to the same file with overlapped IO enabled (the C runtime's own handle is synchronous), tied to the port.
        const HANDLE file = ReOpenFile(reinterpret_cast<HANDLE>(os_handle), for_write ? GENERIC_WRITE : GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
        if (file == INVALID_HANDLE_VALUE) return kInvalidHandle;
        if (CreateIoCompletionPort(file, port().handle(), 0, 0) == nullptr) {
            CloseHandle(file);
            return kInvalidHandle;
        }
        return reinterpret_cast<Handle>(file);
    }

    void close(Handle handle) noexcept { CloseHandle(reinterpret_cast<HANDLE>(handle)); }

    void read(Handle handle, void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        submit(handle, buffer, size, offset, std::move(done), false);
    }

    void write(Handle handle, const void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        submit(handle, const_cast<void *>(buffer), size, offset, std::move(done), true);
    }

    void fsync(Handle handle, bool, Completion done) {
        // FlushFileBuffers has no overlapped form; the system thread pool runs it so no engine thread waits.
        struct Flush {
            HANDLE file;
            Completion done;
        };
        auto *flush = new Flush{reinterpret_cast<HANDLE>(handle), std::move(done)};
        const BOOL queued = QueueUserWorkItem(
            [](void *context) -> DWORD {
                std::unique_ptr<Flush> job(static_cast<Flush *>(context));
                job->done(FlushFileBuffers(job->file) ? 0 : -static_cast<long long>(to_errno(GetLastError())));
                return 0;
            },
            flush, WT_EXECUTEDEFAULT);
        if (!queued) {
            std::unique_ptr<Flush> job(flush);
            job->done(-EAGAIN);
        }
    }

} // namespace SFT::Async::Detail::nativeio

#elif defined(__APPLE__)
// ---- macOS: Grand Central Dispatch --------------------------------------------------------------------------------------------

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <unistd.h>

namespace SFT::Async::Detail::nativeio {

    namespace {
        dispatch_queue_t queue() { return dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0); }
    } // namespace

    bool available() noexcept { return true; }

    Handle duplicate(int fd, bool) noexcept {
        const int copy = ::dup(fd);
        return copy < 0 ? kInvalidHandle : static_cast<Handle>(copy);
    }

    void close(Handle handle) noexcept { ::close(static_cast<int>(handle)); }

    // The transfer runs on the global queue, which owns its own threads: a request costs the engine's IO lane nothing, and many run
    // at once. (dispatch_io would also work but copies through dispatch_data; positional reads straight into the caller's buffer do not.)
    void read(Handle handle, void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        const int fd = static_cast<int>(handle);
        auto *completion = new Completion(std::move(done));
        dispatch_async(queue(), ^{
            ssize_t got;
            do {
                got = ::pread(fd, buffer, size, static_cast<off_t>(offset));
            } while (got < 0 && errno == EINTR);
            const int error = errno;
            std::unique_ptr<Completion> owned(completion);
            (*owned)(got < 0 ? -static_cast<long long>(error) : static_cast<long long>(got));
        });
    }

    void write(Handle handle, const void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        const int fd = static_cast<int>(handle);
        auto *completion = new Completion(std::move(done));
        dispatch_async(queue(), ^{
            ssize_t put;
            do {
                put = ::pwrite(fd, buffer, size, static_cast<off_t>(offset));
            } while (put < 0 && errno == EINTR);
            const int error = errno;
            std::unique_ptr<Completion> owned(completion);
            (*owned)(put < 0 ? -static_cast<long long>(error) : static_cast<long long>(put));
        });
    }

    void fsync(Handle handle, bool, Completion done) {
        const int fd = static_cast<int>(handle);
        auto *completion = new Completion(std::move(done));
        dispatch_async(queue(), ^{
            // F_FULLFSYNC is what makes data durable on macOS; plain fsync stops at the drive's cache.
            const int status = ::fcntl(fd, F_FULLFSYNC) == 0 ? 0 : ::fsync(fd);
            const int error = errno;
            std::unique_ptr<Completion> owned(completion);
            (*owned)(status == 0 ? 0 : -static_cast<long long>(error));
        });
    }

} // namespace SFT::Async::Detail::nativeio

#else
// ---- Everything else (Web, other POSIX): no native async file IO; the IO lane does it -----------------------------------------

namespace SFT::Async::Detail::nativeio {
    bool available() noexcept { return false; }
    Handle duplicate(int, bool) noexcept { return kInvalidHandle; }
    void close(Handle) noexcept {}
    void read(Handle, void *, std::size_t, std::uint64_t, Completion done) { done(-ENOSYS); }
    void write(Handle, const void *, std::size_t, std::uint64_t, Completion done) { done(-ENOSYS); }
    void fsync(Handle, bool, Completion done) { done(-ENOSYS); }
} // namespace SFT::Async::Detail::nativeio

#endif
