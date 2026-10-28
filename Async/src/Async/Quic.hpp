#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Net.hpp>
#include <Async/Size.hpp>
#include <Async/Task.hpp>
#include <Async/Tls.hpp>

namespace SFT::Async {
    using Literals::operator""_KiB;
    using Literals::operator""_MiB;

    // QUIC (RFC 9000) with TLS 1.3: encrypted, multiplexed, connection-oriented transport over UDP -- many independent
    // reliable streams with no head-of-line blocking between them, plus unreliable datagrams (RFC 9221), on one
    // connection that survives IP changes. It is the transport under HTTP/3 and a good fit for games (state on a
    // datagram, chat/inventory on streams) and for anything that wants TCP+TLS without TCP's stalls.
    //
    // Built on ngtcp2 + OpenSSL; needs both at build time (`QuicAvailable()` tells). Security: TLS 1.3 only, the same
    // verify-by-default rules as `TlsClientOptions` (chain + host name, or a pin, or the loud identity-only opt-in),
    // ALPN is mandatory, address validation (Retry) on by default so a spoofed source address cannot make a server flood
    // a victim, hard limits on connections / streams / buffered bytes. 0-RTT is not offered (no replay risk).
    // Continuations run on the network reactor threads, like the rest of Async's networking.

    [[nodiscard]] bool quic_available() noexcept;

    /// Limits both endpoints advertise to each other. Memory is bounded by these: a peer cannot make us buffer more
    /// than the receive windows allow, because credit is only granted as the application reads.
    struct QuicLimits {
        std::chrono::milliseconds idle_timeout{30'000};
        /// Sends a keep-alive probe this often so an idle but wanted connection (and its NAT mapping) stays up.
        /// Zero: off. Should be well under `idle_timeout` and under the NAT's UDP timeout (often 30 s).
        std::chrono::milliseconds keep_alive{0};
        std::chrono::milliseconds handshake_timeout{10'000};
        /// Streams the *peer* may have open at once.
        std::uint32_t max_bidirectional_streams = 100;
        std::uint32_t max_unidirectional_streams = 100;
        /// Receive windows: per stream and for the whole connection.
        std::uint64_t stream_receive_window = 256_KiB;
        std::uint64_t connection_receive_window = 2_MiB;
        /// Unreliable datagrams (RFC 9221). Off: `send_datagram` fails and the peer is told not to send any.
        bool datagrams = true;
        /// Datagrams received but not yet taken by `receive_datagram`; beyond this the oldest is dropped (they are lossy).
        std::size_t datagram_receive_queue = 256;
        /// Datagrams waiting to be sent; beyond this `send_datagram` fails with `LimitExceeded`.
        std::size_t datagram_send_queue = 256;
        /// Unacknowledged bytes one stream may hold in the send buffer before `send` waits (back-pressure).
        std::size_t stream_send_buffer = 1_MiB;
    };

    struct QuicClientOptions : QuicLimits {
        /// ALPN is required (the protocol name both sides agree on, e.g. "my-game/1"). Verification as for TLS over TCP.
        TlsClientOptions tls;
    };

    struct QuicServerOptions : QuicLimits {
        TlsServerOptions tls; ///< certificate + key; `alpn` required
        std::string bind_address; ///< empty: all interfaces
        std::uint16_t port = 0;
        std::size_t max_connections = 1024;
        std::size_t max_connections_per_address = 32;
        /// Send a Retry to every new client: costs one round trip but proves the source address is real, so the server
        /// cannot be used to reflect or amplify traffic at a forged victim address. Turn off only on trusted networks.
        bool require_address_validation = true;
    };

    struct QuicCloseInfo {
        /// 0 for a clean close. Application close: the code the peer gave; transport/TLS failures: `from_peer == false`
        /// carries our own reason in `reason`.
        std::uint64_t code = 0;
        std::string reason;
        bool from_peer = false;
        bool application = true; ///< false: a transport-level error (protocol violation, TLS alert, idle timeout, ...)
        bool timed_out = false;
    };

    struct QuicStats {
        std::chrono::microseconds smoothed_rtt{0};
        std::chrono::microseconds min_rtt{0};
        std::uint64_t bytes_in_flight = 0;
        std::uint64_t congestion_window = 0;
        std::uint64_t packets_lost = 0;
        std::uint64_t bytes_sent = 0;
        std::uint64_t bytes_received = 0;
    };

    namespace Detail {
        struct QuicHandle;
        struct QuicStreamState;
        struct QuicListenerState;
    } // namespace Detail

    /// One direction-ordered byte stream on a connection. Copyable handle; the stream lives until both directions are done.
    /// `receive` resolving to an empty vector means the peer finished its side cleanly.
    class QuicStream {
      public:
        QuicStream() noexcept = default;

        /// QUIC stream id (client-initiated bidirectional streams are 0, 4, 8, ...).
        [[nodiscard]] std::int64_t id() const noexcept;
        [[nodiscard]] bool is_bidirectional() const noexcept;
        [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

        /// Queues `data`; resolves once it fits under `stream_send_buffer`, so a slow peer stalls the sender instead of
        /// ballooning memory. Fails after `finish`, `reset`, or when the connection closes.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> send(std::vector<std::byte> data, CancellationToken cancellation = {});
        /// Ends the sending side cleanly after everything queued (FIN). The receiving side stays usable.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> finish();
        /// 1..`max_bytes` bytes as soon as any arrive; empty at the peer's FIN; `ConnectionReset` if the peer reset the stream
        /// (the error code is in the message). Reading is what grants the peer more flow-control credit.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> receive(std::size_t max_bytes, CancellationToken cancellation = {});

        /// Abruptly stops sending (RESET_STREAM): the peer's reads fail with `error_code`.
        void reset(std::uint64_t error_code = 0);
        /// Asks the peer to stop sending (STOP_SENDING): its writes fail with `error_code`.
        void stop_sending(std::uint64_t error_code = 0);

      private:
        friend class QuicConnection;
        friend struct Detail::QuicHandle;
        std::shared_ptr<Detail::QuicHandle> connection_;
        std::shared_ptr<Detail::QuicStreamState> state_;
    };

    class QuicConnection {
      public:
        QuicConnection() noexcept = default;

        /// Connects and completes the TLS 1.3 handshake. `timeout` of zero uses `options.handshake_timeout`.
        [[nodiscard]] static TaskHandle<std::expected<QuicConnection, IoError>> connect(std::string host, std::uint16_t port, QuicClientOptions options,
                                                                                         CancellationToken cancellation = {});

        [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

        /// Opens a new stream. Waits if the peer's stream limit is reached until it grants more.
        [[nodiscard]] TaskHandle<std::expected<QuicStream, IoError>> open_stream(bool bidirectional = true, CancellationToken cancellation = {});
        /// The next stream the peer opened.
        [[nodiscard]] TaskHandle<std::expected<QuicStream, IoError>> accept_stream(CancellationToken cancellation = {});

        /// Sends one unreliable datagram (it may be lost or reordered, never split). `LimitExceeded` if it is larger than
        /// `max_datagram_size()` or the send queue is full; `Unsupported` if the peer did not enable datagrams.
        [[nodiscard]] std::expected<void, IoError> send_datagram(std::vector<std::byte> data);
        /// The next received datagram.
        [[nodiscard]] TaskHandle<std::expected<std::vector<std::byte>, IoError>> receive_datagram(CancellationToken cancellation = {});
        /// Largest datagram payload that currently fits in one packet (0 if the peer takes none).
        [[nodiscard]] std::size_t max_datagram_size() const;

        /// Closes the connection with an application error code (0 = clean). Pending streams fail.
        void close(std::uint64_t error_code = 0, std::string reason = {});
        /// Completes when the connection ends, whatever the reason.
        [[nodiscard]] TaskHandle<QuicCloseInfo> closed();
        [[nodiscard]] bool is_open() const;

        [[nodiscard]] std::expected<SocketAddress, IoError> peer_address() const;
        [[nodiscard]] std::expected<SocketAddress, IoError> local_address() const;
        /// Negotiated protocol, ALPN and the peer's certificate digest (for mutual-TLS / identity-by-key setups).
        [[nodiscard]] std::expected<TlsInfo, IoError> tls_info() const;
        [[nodiscard]] QuicStats stats() const;

      private:
        friend class QuicListener;
        friend struct Detail::QuicHandle;
        explicit QuicConnection(std::shared_ptr<Detail::QuicHandle> handle) noexcept : handle_(std::move(handle)) {}
        std::shared_ptr<Detail::QuicHandle> handle_;
    };

    struct QuicListenerMetrics {
        std::uint64_t connections_accepted = 0;   ///< handshakes completed
        std::uint64_t connections_active = 0;
        std::uint64_t retries_sent = 0;
        std::uint64_t rejected_limit = 0;         ///< over max_connections / per-address
        std::uint64_t invalid_tokens = 0;
        std::uint64_t packets_dropped = 0;
        std::uint64_t handshakes_failed = 0;
    };

    /// A QUIC server endpoint: one UDP socket, any number of connections.
    class QuicListener {
      public:
        QuicListener() noexcept = default;
        ~QuicListener() noexcept;
        QuicListener(const QuicListener &) = delete;
        QuicListener &operator=(const QuicListener &) = delete;
        QuicListener(QuicListener &&) noexcept = default;
        QuicListener &operator=(QuicListener &&other) noexcept;

        [[nodiscard]] static TaskHandle<std::expected<QuicListener, IoError>> listen(QuicServerOptions options);

        /// The next connection whose handshake completed.
        [[nodiscard]] TaskHandle<std::expected<QuicConnection, IoError>> accept(CancellationToken cancellation = {});
        [[nodiscard]] std::expected<std::uint16_t, IoError> local_port() const;
        [[nodiscard]] QuicListenerMetrics metrics() const noexcept;
        /// Stops accepting and closes every connection.
        void stop() noexcept;

      private:
        std::shared_ptr<Detail::QuicListenerState> state_;
    };

} // namespace SFT::Async
