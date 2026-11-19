#pragma once

// Private: the network side of the engine's io_uring use (Linux). One ring shared by every TCP/UDP socket, a mutex-guarded submit
// path, and one reaper thread that turns completions into callbacks -- so a socket operation in flight occupies no thread and costs
// one submission instead of an epoll wait plus a syscall. The file side lives in Uring.hpp/NativeFileIo; this ring is separate so a
// slow disk can never delay a packet.
//
// `available()` says whether this machine can run the ring (whether to use it is the caller's policy: see net_uring() in NetCore.hpp). It is decided once, at run time: it is false when liburing is not built in, the kernel is too old for the opcodes used
// (cancel-by-descriptor needs 5.19), the kernel or container policy refuses a ring, or STURDY_ASYNC_NO_IO_URING is set. Every user
// of this header must then use the Asio (epoll) path, which is always compiled in: the same binary runs on both kinds of machine.
//
// Descriptors are the caller's. An operation in flight keeps the kernel's reference to the open file, so before closing a socket
// call `cancel(fd)` and let the cancelled operations complete (they report -ECANCELED); closing alone would leave them pending.

#include <cstddef>
#include <cstdint>
#include <functional>

struct msghdr;
struct sockaddr;

namespace SFT::Async::Detail::netring {

    /// `result` is the operation's result (byte count, a new descriptor, 0) or a negative errno. Runs on the reaper thread: keep it
    /// short and hop to the owner's executor. (`std::function`, so a move-only owner is boxed in a shared_ptr by its caller.)
    using Completion = std::function<void(int result)>;

    [[nodiscard]] bool available() noexcept;

    /// The buffer (or message, or address storage) must stay valid until `done` runs.
    void recv(int fd, void *buffer, std::size_t size, Completion done);
    void send(int fd, const void *buffer, std::size_t size, Completion done);
    void recvmsg(int fd, msghdr *message, Completion done);
    void sendmsg(int fd, const msghdr *message, Completion done);
    /// `done` receives the accepted descriptor. `address`/`length` may be null.
    void accept(int fd, sockaddr *address, unsigned *length, Completion done);
    void connect(int fd, const sockaddr *address, unsigned length, Completion done);

    /// Cancels every operation in flight on `fd`. Asynchronous: the cancelled operations complete with -ECANCELED. Safe to call with
    /// nothing in flight.
    void cancel(int fd) noexcept;

} // namespace SFT::Async::Detail::netring
