#pragma once

// Private: the platform's native asynchronous positional file IO, behind one interface so `Async::File` has a fast path on every
// platform that has one. Linux: io_uring (see Uring.hpp). Windows: overlapped IO completed through an I/O completion port, with one
// reaper thread. macOS: Grand Central Dispatch's global queue running positional reads and writes. In each case a request in flight
// occupies no thread of the engine's IO lane, and `available()` is false where the platform (or the kernel's policy) offers
// nothing, in which case callers use the IO lane.
//
// Every operation gets its own private handle (`duplicate`), so `File::close()` cannot pull a descriptor out from under a request in
// flight. Completions run on the backend's own thread: keep them short, and hop with `then<Rt>` before doing real work.

#include <cstddef>
#include <cstdint>
#include <functional>

namespace SFT::Async::Detail::nativeio {

    using Handle = std::intptr_t;
    inline constexpr Handle kInvalidHandle = -1;

    /// `result` is the byte count (0 for a completed fsync) or a negative errno value.
    using Completion = std::function<void(long long result)>;

    [[nodiscard]] bool available() noexcept;

    /// A private handle for one operation on the file behind the C runtime descriptor `fd`; `kInvalidHandle` when it cannot be made.
    [[nodiscard]] Handle duplicate(int fd, bool for_write) noexcept;
    void close(Handle handle) noexcept;

    /// The handle and buffer must stay valid until `done` runs.
    void read(Handle handle, void *buffer, std::size_t size, std::uint64_t offset, Completion done);
    void write(Handle handle, const void *buffer, std::size_t size, std::uint64_t offset, Completion done);
    void fsync(Handle handle, bool data_only, Completion done);

} // namespace SFT::Async::Detail::nativeio
