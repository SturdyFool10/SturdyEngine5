#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <Async/Http.hpp>
#include <Async/IoError.hpp>
#include <Async/Size.hpp>
#include <Async/Task.hpp>
#include <Async/WebSocket.hpp>

namespace SFT::Async {
    using Literals::operator""_KiB;
    using Literals::operator""_MiB;

    struct HttpServerOptions {
        std::string bind_address = "0.0.0.0";
        std::uint16_t port = 0;
        unsigned io_threads = 0;
        unsigned handler_threads = 0;
        unsigned max_connections = 256;
        std::uint64_t max_request_body_bytes = 1_MiB;
        std::uint32_t max_request_header_bytes = 32_KiB;
        std::uint64_t max_response_body_bytes = 8_MiB;
        std::uint32_t shutdown_timeout_ms = 5000;
        std::string tls_certificate_chain_file;
        std::string tls_private_key_file;

        /// Limits and liveness policy for sockets upgraded through `on_websocket`.
        WebSocketOptions websocket;
        /// If non-empty, an upgrade request carrying an Origin header must match one of these exactly.
        std::vector<std::string> websocket_allowed_origins;
        /// Supported subprotocols, most preferred first.
        std::vector<std::string> websocket_subprotocols;
        /// Upgraded sockets alive at once (they no longer count against `max_connections`).
        std::size_t max_websockets = 1024;

        /// Request bodies whose Content-Length exceeds this many bytes are written to a temporary file instead of memory
        /// (handlers see `HttpRequestMessage::body_file`), so large uploads cost disk, not RAM. 0 disables spooling.
        std::uint64_t spool_requests_above = 0;
        /// Upper bound for spooled bodies (larger uploads get 413).
        std::uint64_t max_spooled_body_bytes = 1024ull * 1024 * 1024;
        /// Connections allowed from one client address at once; 0 means no limit. A single host cannot then use up
        /// `max_connections`.
        unsigned max_connections_per_ip = 0;

        /// Offer HTTP/2 over TLS (ALPN "h2"; clients that do not ask for it still get HTTP/1.1). Plain-text HTTP/2 is not
        /// offered. Ignored when the build has no nghttp2.
        bool enable_http2 = true;
        /// Streams a single HTTP/2 connection may have in flight at once.
        unsigned http2_max_concurrent_streams = 100;

        /// Also serve HTTP/3 (QUIC) on a UDP port, and advertise it to HTTP/1.1 and HTTP/2 clients with `Alt-Svc`. Needs TLS
        /// and a build with ngtcp2 + nghttp3; `start()` fails if it was asked for and is not possible.
        bool enable_http3 = false;
        /// UDP port for HTTP/3; 0 uses the same number as the TCP port (what browsers expect for `Alt-Svc: h3=":443"`).
        std::uint16_t http3_port = 0;
        unsigned http3_max_concurrent_streams = 100;
        /// QUIC connections allowed at once / from one address (see `QuicServerOptions`).
        std::size_t http3_max_connections = 1024;
        std::size_t http3_max_connections_per_address = 64;
    };

    /// Counters since the server started (monotonic, except `active_*`).
    struct HttpServerMetrics {
        std::uint64_t connections_accepted = 0;
        std::uint64_t connections_rejected_limit = 0;   ///< over `max_connections`
        std::uint64_t connections_rejected_per_ip = 0;  ///< over `max_connections_per_ip`
        std::uint64_t requests = 0;
        std::uint64_t responses_2xx = 0;
        std::uint64_t responses_3xx = 0;
        std::uint64_t responses_4xx = 0;
        std::uint64_t responses_5xx = 0;
        std::uint64_t websocket_upgrades = 0;
        std::uint64_t active_connections = 0;
        std::uint64_t active_websockets = 0;
    };

    namespace Detail {
        class HttpServerState;
    }

    /// Native HTTP/1.1 server. Request handlers run on a separate bounded-count handler pool,
    /// never on the Asio reactor threads. Set both TLS files to enable HTTPS with OpenSSL.
    class HttpServer {
      public:
        using Handler = std::function<HttpResponseMessage(HttpRequestMessage)>;
        using AsyncHandler = std::function<TaskHandle<HttpResponseMessage>(HttpRequestMessage)>;
        using WebSocketHandler = std::function<void(WebSocket, HttpRequestMessage)>;

        HttpServer(HttpServerOptions options, Handler handler);
        HttpServer(HttpServerOptions options, AsyncHandler handler);
        ~HttpServer();

        HttpServer(const HttpServer &) = delete;
        HttpServer &operator=(const HttpServer &) = delete;
        HttpServer(HttpServer &&) noexcept;
        HttpServer &operator=(HttpServer &&) noexcept;

        /// Serves WebSocket upgrades for requests whose path (without query) equals `path`, on the same port as
        /// HTTP (and over TLS when the server uses it). `handler` runs on the handler pool with the open socket and
        /// the upgrade request; the server does not track the socket afterwards, so close it when done.
        /// Call before `start()`.
        void on_websocket(std::string path, WebSocketHandler handler);

        /// Opens the listener and starts asynchronous accepts.
        [[nodiscard]] std::expected<void, IoError> start();
        /// Stops accepting and closes active sessions, then joins runtime threads. The session drain
        /// is bounded by `shutdown_timeout_ms`; a user handler that blocks indefinitely can still
        /// delay shutdown. Do not destroy the server from inside one of its request handlers.
        void stop() noexcept;
        [[nodiscard]] bool is_running() const noexcept;
        [[nodiscard]] std::expected<std::uint16_t, IoError> local_port() const;
        [[nodiscard]] bool uses_tls() const noexcept;
        /// Whether this server will negotiate HTTP/2 (TLS configured, enabled, and nghttp2 available).
        [[nodiscard]] bool offers_http2() const noexcept;
        /// The UDP port HTTP/3 is served on, or an error when it is not enabled / not running.
        [[nodiscard]] std::expected<std::uint16_t, IoError> http3_local_port() const;
        [[nodiscard]] HttpServerMetrics metrics() const noexcept;

      private:
        std::unique_ptr<Detail::HttpServerState> state_;
    };

} // namespace SFT::Async
