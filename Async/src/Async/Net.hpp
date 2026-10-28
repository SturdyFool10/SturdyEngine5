#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Task.hpp>
#include <Async/Tls.hpp>

namespace SFT::Async {

    using i64 = std::int64_t;
    using u16 = std::uint16_t;

    /// Every operation in this header runs on the shared network reactor (epoll / io_uring / kqueue / IOCP
    /// through Boost.Asio) and returns a `TaskHandle` that the reactor completes: pending operations hold
    /// no thread, so tens of thousands of idle connections are cheap.
    ///
    /// Conventions
    ///  * Failures are values (`std::expected<_, IoError>`). A cancelled operation instead fails the
    ///    handle with `IoOperationCancelled`.
    ///  * Continuations (`co_await`, `on_done`) run on a reactor thread. Hop to a scheduler with
    ///    `then<Rt>` / `co_await yield_to<Rt>()` before doing real work, and never `wait()` on a handle
    ///    from a reactor thread.
    ///  * Operations of one kind on one object (several `receive`s, several `send`s) execute strictly in
    ///    the order they were issued; a `send` and a `receive` may be in flight together.
    ///  * Closing an object completes everything pending on it with `IoErrorCode::Closed`.
    ///  * Hostile peers cannot force unbounded memory: reads grow with bytes actually received.

    class TcpConnection;
    class TcpListener;
    class UdpSocket;

    /// A numeric network endpoint (IPv4 dotted quad or IPv6 text) and port.
    struct SocketAddress {
        std::string host;
        u16 port = 0;
    };

    /// One received UDP datagram and where it came from.
    struct UdpDatagram {
        std::vector<std::byte> data;
        std::string sender_host;
        u16 sender_port = 0;
    };

    /// Reactor sizing. Optional; call before the first network object is created. Returns false if too late.
    [[nodiscard]] bool configure_network_reactor(unsigned thread_count);

    /// Resolves a host name to numeric addresses (DNS may take seconds; it never blocks a worker).
    [[nodiscard]] TaskHandle<std::expected<std::vector<SocketAddress>, IoError>> resolve_host(std::string host, u16 port = 0);

    namespace Detail {
        struct StreamCore;
        struct ListenerCore;
        struct UdpCore;
    } // namespace Detail

    /// A TCP stream, optionally wrapped in TLS. Move-only; destroying it closes the connection.
    class TcpConnection {
      public:
        TcpConnection() noexcept = default;
        ~TcpConnection() noexcept;
        TcpConnection(const TcpConnection &) = delete;
        TcpConnection &operator=(const TcpConnection &) = delete;
        TcpConnection(TcpConnection &&other) noexcept = default;
        TcpConnection &operator=(TcpConnection &&other) noexcept;

        /// Connects, trying each resolved address in turn. `timeout` bounds the whole attempt including DNS
        /// and (zero means 30 s, the default cap, so nothing waits forever).
        [[nodiscard]] static TaskHandle<std::expected<TcpConnection, IoError>> connect(
            std::string host, u16 port, std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(),
            CancellationToken cancellation = {});

        /// Connects and performs a verified TLS handshake (see `TlsClientOptions`: chain + host name are
        /// always checked; a pinned certificate replaces chain validation). `timeout` covers connect and handshake.
        [[nodiscard]] static TaskHandle<std::expected<TcpConnection, IoError>> connect_tls(
            std::string host, u16 port, TlsClientOptions tls = {},
            std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(), CancellationToken cancellation = {});

        /// Queues all of `data` for sending; resolves to its size once every byte is handed to the OS.
        /// Cancelling a send that already started may leave a partial message on the wire.
        [[nodiscard]] TaskHandle<std::expected<std::size_t, IoError>> send(std::vector<std::byte> data,
                                                                           CancellationToken cancellation = {});
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> send_all(std::vector<std::byte> data,
                                                                        CancellationToken cancellation = {});

        /// Resolves with 1..`max_bytes` bytes as soon as any arrive, or empty at end of stream.
        /// Cancelling does not lose data: bytes that arrive meanwhile are kept for the next receive.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> receive(std::size_t max_bytes,
                                                                                         CancellationToken cancellation = {});
        /// Exactly `byte_count` bytes; `UnexpectedEof` if the peer closes first.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> receive_exact(
            std::size_t byte_count, CancellationToken cancellation = {});
        /// Bytes up to and including `delimiter` (a line, a header block). `LimitExceeded` once more than
        /// `max_bytes` accumulate without finding it; `UnexpectedEof` if the peer closes first.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> receive_until(
            std::string delimiter, std::size_t max_bytes, CancellationToken cancellation = {});

        /// Upgrades this open connection to TLS in place (client role): after an HTTP `CONNECT` tunnel, a STARTTLS
        /// exchange, or any protocol that negotiates security mid-stream. The connection must be quiet -- no pending
        /// receives or sends and no buffered unread bytes -- and must not be used until the handshake completes. Chain and
        /// host name are verified as in `connect_tls` (`host` defaults the expected name).
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> start_tls_client(std::string host, TlsClientOptions tls = {},
                                                                                CancellationToken cancellation = {});
        /// Server-role counterpart of `start_tls_client`.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> start_tls_server(TlsServerOptions tls);

        /// Half-close: tells the peer no more data is coming (TLS: sends close_notify) while reads continue.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> shutdown_write();
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> set_nodelay(bool enabled);
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> set_keepalive(bool enabled);

        [[nodiscard]] bool is_open() const noexcept;
        void close() noexcept;

        [[nodiscard]] std::expected<SocketAddress, IoError> peer_address() const;
        [[nodiscard]] std::expected<SocketAddress, IoError> local_address() const;
        [[nodiscard]] bool is_tls() const noexcept;
        /// Negotiated protocol, cipher, ALPN and the peer's certificate digest; fails on plain connections.
        [[nodiscard]] std::expected<TlsInfo, IoError> tls_info() const;

        /// Resolves a host name to numeric addresses; same as `resolve_host`.
        [[nodiscard]] static TaskHandle<std::expected<std::vector<SocketAddress>, IoError>> resolve(std::string host, u16 port = 0) {
            return resolve_host(std::move(host), port);
        }

      private:
        friend struct Detail::StreamCore;
        explicit TcpConnection(std::shared_ptr<Detail::StreamCore> core) noexcept : core_(std::move(core)) {}

        std::shared_ptr<Detail::StreamCore> core_;
    };

    /// Binds and accepts. Connections are accepted (and, for TLS, handshaken) ahead of time into a bounded
    /// ready queue, so one slow handshake never stalls the others; when the queue is full the kernel backlog
    /// applies back-pressure. Keep calling `accept` in a loop (or `co_await` it).
    class TcpListener {
      public:
        TcpListener() noexcept = default;
        ~TcpListener() noexcept;
        TcpListener(const TcpListener &) = delete;
        TcpListener &operator=(const TcpListener &) = delete;
        TcpListener(TcpListener &&other) noexcept = default;
        TcpListener &operator=(TcpListener &&other) noexcept;

        /// Binds `host:port` (empty host = all interfaces, port 0 = OS-chosen) and starts listening.
        [[nodiscard]] static TaskHandle<std::expected<TcpListener, IoError>> listen(std::string host, u16 port, int backlog = 1024);
        /// Same, but every accepted connection completes a TLS server handshake first.
        /// Failed handshakes are dropped silently (they never reach `accept`).
        [[nodiscard]] static TaskHandle<std::expected<TcpListener, IoError>> listen_tls(std::string host, u16 port, TlsServerOptions tls,
                                                                                         int backlog = 1024);

        [[nodiscard]] TaskHandle<std::expected<TcpConnection, IoError>> accept(CancellationToken cancellation = {});

        [[nodiscard]] bool is_open() const noexcept;
        void close() noexcept;
        [[nodiscard]] std::expected<u16, IoError> local_port() const;
        [[nodiscard]] std::expected<SocketAddress, IoError> local_address() const;

      private:
        friend struct Detail::ListenerCore;
        explicit TcpListener(std::shared_ptr<Detail::ListenerCore> core) noexcept : core_(std::move(core)) {}
        std::shared_ptr<Detail::ListenerCore> core_;
    };

    struct UdpOptions {
        /// SO_REUSEADDR: several sockets may bind the same port (needed for multicast listeners and discovery).
        bool reuse_address = false;
        bool broadcast = false;
        /// Kernel buffer sizes in bytes; 0 keeps the OS default.
        std::size_t receive_buffer = 0;
        std::size_t send_buffer = 0;
    };

    /// A bound UDP socket. Datagram boundaries are preserved: one receive returns one datagram.
    class UdpSocket {
      public:
        UdpSocket() noexcept = default;
        ~UdpSocket() noexcept;
        UdpSocket(const UdpSocket &) = delete;
        UdpSocket &operator=(const UdpSocket &) = delete;
        UdpSocket(UdpSocket &&other) noexcept = default;
        UdpSocket &operator=(UdpSocket &&other) noexcept;

        /// Binds `host:port` (empty host = all interfaces, port 0 = OS-chosen). IPv4 and IPv6 hosts both work.
        [[nodiscard]] static TaskHandle<std::expected<UdpSocket, IoError>> bind(std::string host, u16 port, UdpOptions options = {});

        /// Fixes the default peer: `send` goes there and the kernel drops datagrams from anyone else.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> connect(std::string host, u16 port);
        /// Sends one datagram to the connected peer; resolves to the byte count (datagrams are never split).
        [[nodiscard]] TaskHandle<std::expected<std::size_t, IoError>> send(std::vector<std::byte> data);
        /// Sends one datagram. Literal IPs are used directly; host names are resolved (and cached).
        [[nodiscard]] TaskHandle<std::expected<std::size_t, IoError>> send_to(std::string host, u16 port, std::vector<std::byte> data);
        /// Waits for the next datagram of at most `max_bytes` bytes (longer ones are truncated). A datagram
        /// that arrives while its receive was being cancelled is kept for the next one.
        [[nodiscard]] TaskHandle<std::expected<UdpDatagram, IoError>> receive_from(std::size_t max_bytes,
                                                                                    CancellationToken cancellation = {});

        [[nodiscard]] TaskHandle<std::expected<void, IoError>> set_broadcast(bool enabled);
        /// Joins an IPv4/IPv6 multicast `group`; `interface_address` (empty = default) picks the NIC.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> join_multicast(std::string group, std::string interface_address = {});
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> leave_multicast(std::string group, std::string interface_address = {});
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> set_multicast_loopback(bool enabled);
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> set_multicast_ttl(unsigned hops);

        [[nodiscard]] bool is_open() const noexcept;
        void close() noexcept;
        [[nodiscard]] std::expected<u16, IoError> local_port() const;
        [[nodiscard]] std::expected<SocketAddress, IoError> local_address() const;

      private:
        friend struct Detail::UdpCore;
        explicit UdpSocket(std::shared_ptr<Detail::UdpCore> core) noexcept : core_(std::move(core)) {}
        std::shared_ptr<Detail::UdpCore> core_;
    };

} // namespace SFT::Async
