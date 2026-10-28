#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/Http.hpp>
#include <Async/IoError.hpp>
#include <Async/Net.hpp>
#include <Async/Size.hpp>
#include <Async/Task.hpp>
#include <Async/Tls.hpp>

namespace SFT::Async {
    using Literals::operator""_KiB;
    using Literals::operator""_MiB;

    /// Per-connection limits and liveness policy. The defaults are safe for untrusted peers.
    struct WebSocketOptions {
        /// Largest accepted incoming message (after reassembling fragments). Bigger ones close the socket with
        /// code 1009 and fail pending receives with `LimitExceeded`.
        std::size_t max_message_bytes = 1_MiB;
        /// Bytes that may sit in the send queue; a `send_*` that would exceed it fails with `LimitExceeded`
        /// instead of growing memory without bound. Await sends to be paced by the peer automatically.
        std::size_t max_queued_send_bytes = 4_MiB;
        /// Messages read ahead of the application. When full, reading pauses and TCP back-pressure reaches the peer.
        std::size_t inbox_messages = 16;
        std::chrono::milliseconds handshake_timeout{10'000};
        /// No inbound traffic for this long drops the connection; pings are sent at half this interval to keep
        /// healthy idle links alive. Zero disables liveness checks.
        std::chrono::milliseconds idle_timeout{60'000};
        /// Negotiate permessage-deflate (RFC 7692). Off by default; `max_message_bytes` bounds the *inflated* size, so a
        /// small compressed "bomb" is cut off the same way a big plain message is. Uses per-message contexts (no
        /// cross-message state), trading some ratio for bounded memory per connection.
        bool compression = false;
    };

    struct WebSocketMessage {
        bool is_text = false;
        std::vector<std::byte> data;
        [[nodiscard]] std::string text() const { return std::string(reinterpret_cast<const char *>(data.data()), data.size()); }
    };

    /// What the peer asked for in the upgrade request (server side) or what the server selected (client side).
    struct WebSocketRequest {
        std::string target;                 ///< path + query, e.g. "/chat?room=1"
        std::vector<HttpHeader> headers;
        std::string origin;                 ///< Origin header, empty for non-browser clients
        std::string subprotocol;            ///< the negotiated Sec-WebSocket-Protocol, empty if none
    };

    struct WebSocketCloseInfo {
        std::uint16_t code = 0;             ///< RFC 6455 close code (1000 normal, 1001 going away, ...)
        std::string reason;
    };

    struct WebSocketClientOptions {
        WebSocketOptions socket;
        /// Used for wss:// URLs (certificate chain + host name verified by default).
        TlsClientOptions tls;
        /// Extra request headers (Authorization, Cookie, Origin ...). Hop-by-hop upgrade headers are managed.
        std::vector<HttpHeader> headers;
        /// Offered subprotocols, most preferred first.
        std::vector<std::string> subprotocols;
        /// Covers resolve + connect + TLS + upgrade. Zero means 30 s.
        std::chrono::milliseconds timeout{0};
    };

    struct WebSocketServerOptions {
        std::string bind_address;           ///< empty: all interfaces
        std::uint16_t port = 0;             ///< 0: OS-chosen
        WebSocketOptions socket;
        /// Serve wss:// with this identity when set; plain ws:// otherwise.
        std::optional<TlsServerOptions> tls;
        /// If non-empty, a request carrying an Origin header must match one of these exactly (browser
        /// cross-site hijacking defence). Requests without an Origin header (native clients) are unaffected.
        std::vector<std::string> allowed_origins;
        /// Request paths served (matched against the target without its query). Empty: any path.
        std::vector<std::string> paths;
        /// Supported subprotocols, most preferred first; the first one the client offers is selected.
        std::vector<std::string> subprotocols;
        /// Upgrade-request header size cap.
        std::size_t max_request_header_bytes = 16_KiB;
        /// Live sockets allowed at once; further connections are closed immediately.
        std::size_t max_connections = 1024;
    };

    namespace Detail {
        class WsOps;
        struct WsFactory;
        struct WsServerCore;
    } // namespace Detail

    /// A WebSocket connection (RFC 6455), server- or client-side, plain or over TLS. Move-only; destroying it
    /// aborts the connection (use `close()` first for a graceful goodbye).
    ///
    /// Reading is continuous behind a bounded inbox, so ping/pong and close frames are handled even while the
    /// application is busy. Operations of one kind execute in the order they were issued.
    class WebSocket {
      public:
        WebSocket() noexcept = default;
        ~WebSocket() noexcept;
        WebSocket(const WebSocket &) = delete;
        WebSocket &operator=(const WebSocket &) = delete;
        WebSocket(WebSocket &&) noexcept = default;
        WebSocket &operator=(WebSocket &&other) noexcept;

        /// Connects to `ws://host[:port]/path` or `wss://...` and performs the upgrade.
        [[nodiscard]] static TaskHandle<std::expected<WebSocket, IoError>> connect(std::string url, WebSocketClientOptions options = {},
                                                                                    CancellationToken cancellation = {});

        /// Next data message. After the connection ends the buffered messages drain first, then every receive
        /// fails with `Closed` (see `close_info()`) or the error that ended the connection.
        [[nodiscard]] TaskHandle<std::expected<WebSocketMessage, IoError>> receive(CancellationToken cancellation = {});

        [[nodiscard]] TaskHandle<std::expected<void, IoError>> send_text(std::string text);
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> send_binary(std::vector<std::byte> data);
        /// Sends a ping (payload up to 125 bytes); the pong is consumed internally.
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> ping(std::vector<std::byte> payload = {});

        /// Graceful close: queued messages go out first, then a close frame, then the peer's reply is awaited
        /// (bounded by the handshake timeout).
        [[nodiscard]] TaskHandle<std::expected<void, IoError>> close(std::uint16_t code = 1000, std::string reason = {});
        /// Drops the transport immediately.
        void abort() noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        [[nodiscard]] const WebSocketRequest &request() const noexcept;
        /// Close code/reason once the connection has ended through a close handshake.
        [[nodiscard]] std::optional<WebSocketCloseInfo> close_info() const;
        [[nodiscard]] std::expected<SocketAddress, IoError> peer_address() const;
        [[nodiscard]] std::expected<TlsInfo, IoError> tls_info() const;

      private:
        friend struct Detail::WsFactory;
        explicit WebSocket(std::shared_ptr<Detail::WsOps> ops) noexcept : ops_(std::move(ops)) {}
        std::shared_ptr<Detail::WsOps> ops_;
    };

    /// Accepts WebSocket connections: TCP accept, optional TLS, HTTP upgrade (with Origin / path / subprotocol
    /// checks and a header-size cap) all happen ahead of `accept`, concurrently, so one slow or hostile client
    /// cannot delay the others. Failed upgrades are answered with an HTTP error and dropped.
    class WebSocketServer {
      public:
        WebSocketServer() noexcept = default;
        ~WebSocketServer() noexcept;
        WebSocketServer(const WebSocketServer &) = delete;
        WebSocketServer &operator=(const WebSocketServer &) = delete;
        WebSocketServer(WebSocketServer &&) noexcept = default;
        WebSocketServer &operator=(WebSocketServer &&other) noexcept;

        [[nodiscard]] static TaskHandle<std::expected<WebSocketServer, IoError>> listen(WebSocketServerOptions options);

        [[nodiscard]] TaskHandle<std::expected<WebSocket, IoError>> accept(CancellationToken cancellation = {});
        [[nodiscard]] std::expected<std::uint16_t, IoError> local_port() const;
        [[nodiscard]] bool is_open() const noexcept;
        /// Stops accepting; existing sockets stay alive until closed by their owners.
        void close() noexcept;

      private:
        std::shared_ptr<Detail::WsServerCore> core_;
    };

} // namespace SFT::Async
