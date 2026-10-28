#pragma once

// Private: a minimal io_uring front end for positional file IO (Linux, optional liburing). One ring, a mutex-guarded
// submit path, and one reaper thread that does nothing but turn completions into callbacks -- so a file read costs no
// worker thread while it is in flight. `available()` is false when liburing is not built in, the kernel refuses to
// create a ring (old kernel, seccomp, container policy) or STURDY_ASYNC_NO_IO_URING is set; callers then use the
// blocking IO lane.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace SFT::Async::Detail::uring {

    /// `result` is the byte count (or 0 for fsync), or a negative errno.
    using Completion = std::function<void(long long result)>;

    [[nodiscard]] bool available() noexcept;

    /// The descriptor and buffer must stay valid until `done` runs. `done` runs on the reaper thread: keep it short.
    void read(int fd, void *buffer, std::size_t size, std::uint64_t offset, Completion done);
    void write(int fd, const void *buffer, std::size_t size, std::uint64_t offset, Completion done);
    void fsync(int fd, bool data_only, Completion done);

} // namespace SFT::Async::Detail::uring
