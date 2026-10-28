#pragma once

// A flat, language-neutral face for everything in Async that a foreign language needs: the C ABI (`FFI/`) and the Rust
// shim (`Rust/sturdy-sys`) are both thin translations of this one header, so the two cannot drift apart and the
// behaviour (limits, cancellation, error codes) is defined once.
//
// Shape:
//   * Objects (`ObjectPtr`) are opaque, reference-counted, and carry a kind name ("tcp", "websocket", "quic_stream", ...).
//     Every function that takes one checks the kind and throws `BindingError` (InvalidArgument) on a mismatch.
//   * Anything that waits returns an `Op`: a pending operation you can poll, wait on (with a timeout), cancel, or attach a
//     completion callback to. When it finishes it holds a `Value` -- status, error, bytes, text, a number, key/value
//     fields and/or a new object -- whose meaning is documented per function. One completion model for every protocol.
//   * Plain types only (std::string, vectors of bytes): nothing here exposes Asio, Beast or a coroutine.
//
// Thread-safety: all functions may be called from any thread. Completion callbacks run on a network-reactor, timer or worker
// thread: keep them short, do not wait on an Op from inside one, and copy what you need before returning.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>

namespace SFT::Async::Binding {

    using Bytes = std::vector<std::uint8_t>;
    using Fields = std::vector<std::pair<std::string, std::string>>;

    /// Thrown for misuse that is detected synchronously (wrong object kind, bad argument). Asynchronous failures are
    /// reported through the Op instead.
    class BindingError : public std::runtime_error {
      public:
        BindingError(IoErrorCode code, const std::string &message) : std::runtime_error(message), code_(code) {}
        [[nodiscard]] IoErrorCode code() const noexcept { return code_; }

      private:
        IoErrorCode code_;
    };

    class Object {
      public:
        virtual ~Object() = default;
        [[nodiscard]] virtual const char *kind() const noexcept = 0;
    };
    using ObjectPtr = std::shared_ptr<Object>;

    /// The two ends of a channel, oneshot, watch or broadcast (`sender`/`receiver`), or of an in-memory pipe (both are "pipe_end").
    struct ChannelEnds {
        ObjectPtr sender;
        ObjectPtr receiver;
    };

    enum class OpStatus { Pending, Ok, Failed, Cancelled };

    struct Value {
        OpStatus status = OpStatus::Pending;
        IoErrorCode code = IoErrorCode::Unknown; ///< meaningful when status == Failed
        std::string error;
        Bytes bytes;
        std::string text;
        std::int64_t number = 0;
        Fields fields;
        ObjectPtr object;
    };

    class Op {
      public:
        Op();
        [[nodiscard]] bool done() const noexcept;
        [[nodiscard]] OpStatus status() const noexcept;
        /// Waits up to `timeout_ms` (negative: forever). True when the operation has finished.
        bool wait(std::int64_t timeout_ms);
        /// Asks the operation to stop; it then finishes with `Cancelled` (or with its result if it was already done).
        void cancel() noexcept;
        [[nodiscard]] CancellationToken token() const;
        /// Runs `fn` when the operation finishes (immediately, on this thread, if it already has).
        void on_complete(std::function<void()> fn);
        /// Valid once `done()`: the result stays immutable afterwards.
        [[nodiscard]] const Value &result() const noexcept { return value_; }

        // ---- used by the implementation ----
        void complete(Value value);
        void fail(const IoError &error);
        void fail(IoErrorCode code, std::string message);
        void mark_cancelled();

      private:
        struct State;
        std::shared_ptr<State> state_;
        Value value_;
    };
    using OpPtr = std::shared_ptr<Op>;

    /// Waits for the first of `ops` to finish; returns its index, or -1 on timeout.
    [[nodiscard]] int wait_any(const std::vector<OpPtr> &ops, std::int64_t timeout_ms);

    // ---------------------------------------------------------------------------------------------------------------
    // Capabilities
    // ---------------------------------------------------------------------------------------------------------------
    enum FeatureBits : std::uint32_t {
        FeatureQuic = 1u << 0,
        FeatureHttp2 = 1u << 1,
        FeatureHttp3 = 1u << 2,
        FeatureIoUring = 1u << 3,
        FeatureProcess = 1u << 4,
        FeatureGzip = 1u << 5,
        FeatureNativeFileWatch = 1u << 6,
    };
    /// Which optional parts this build contains / this machine supports.
    [[nodiscard]] std::uint32_t features() noexcept;

    // ---------------------------------------------------------------------------------------------------------------
    // TLS
    // ---------------------------------------------------------------------------------------------------------------
    struct TlsClientSpec {
        std::string server_name;
        bool use_system_roots = true;
        std::string ca_pem;
        std::vector<std::string> pinned_sha256;
        bool identify_peer_by_certificate_only = false; ///< loud opt-in: the caller must compare `peer_sha256` itself
        std::string client_certificate_pem;
        std::string client_private_key_pem;
        std::vector<std::string> alpn;
        std::uint32_t handshake_timeout_ms = 10'000;
    };
    struct TlsServerSpec {
        std::string certificate_pem;
        std::string private_key_pem;
        std::string client_ca_pem;
        bool require_client_certificate = false;
        bool accept_any_client_certificate = false; ///< loud opt-in (peer-to-peer identity by key)
        std::vector<std::string> pinned_client_sha256;
        std::vector<std::string> alpn;
        std::uint32_t handshake_timeout_ms = 10'000;
    };
    struct Identity {
        std::string certificate_pem;
        std::string private_key_pem;
        std::string sha256;
    };
    [[nodiscard]] Identity generate_identity(const std::string &common_name, unsigned valid_days);
    [[nodiscard]] std::string certificate_sha256(const std::string &certificate_pem);

    // ---------------------------------------------------------------------------------------------------------------
    // Timers, signals, DNS, STUN
    // ---------------------------------------------------------------------------------------------------------------
    /// Finishes after `ms` (Ok), or Cancelled.
    [[nodiscard]] OpPtr sleep(std::int64_t ms);
    /// Finishes with number = the signal (SIGINT/SIGTERM) that arrived.
    [[nodiscard]] OpPtr shutdown_signal();
    /// fields = ("address", numeric host) per result.
    [[nodiscard]] OpPtr resolve_host(const std::string &host);
    /// text = public host, number = public port.
    [[nodiscard]] OpPtr stun_query(const std::string &server, std::uint16_t port, std::int64_t timeout_ms);

    // ---------------------------------------------------------------------------------------------------------------
    // TCP / TLS streams  (kinds "tcp", "tcp_listener")
    // ---------------------------------------------------------------------------------------------------------------
    /// object = "tcp". `tls` null: plain TCP.
    [[nodiscard]] OpPtr tcp_connect(const std::string &host, std::uint16_t port, const TlsClientSpec *tls, std::int64_t timeout_ms);
    /// object = "tcp_listener". Completes immediately. `backlog` 0: 1024.
    [[nodiscard]] OpPtr tcp_listen(const std::string &host, std::uint16_t port, const TlsServerSpec *tls, std::int32_t backlog);
    [[nodiscard]] OpPtr tcp_accept(const ObjectPtr &listener);            ///< object = "tcp"
    [[nodiscard]] std::uint16_t tcp_listener_port(const ObjectPtr &listener);
    /// "host:port" the listener is bound to.
    [[nodiscard]] std::string tcp_listener_address(const ObjectPtr &listener);
    void tcp_listener_close(const ObjectPtr &listener);
    [[nodiscard]] bool tcp_listener_is_open(const ObjectPtr &listener);
    [[nodiscard]] OpPtr tcp_send(const ObjectPtr &connection, Bytes data); ///< number = bytes sent
    [[nodiscard]] OpPtr tcp_receive(const ObjectPtr &connection, std::size_t max_bytes);        ///< bytes (empty = end of stream)
    [[nodiscard]] OpPtr tcp_receive_exact(const ObjectPtr &connection, std::size_t count);      ///< bytes
    [[nodiscard]] OpPtr tcp_receive_until(const ObjectPtr &connection, const std::string &delimiter, std::size_t max_bytes); ///< bytes incl. delimiter
    [[nodiscard]] OpPtr tcp_shutdown_write(const ObjectPtr &connection);
    /// Upgrades an open connection to TLS in place (STARTTLS / CONNECT tunnels).
    [[nodiscard]] OpPtr tcp_start_tls_client(const ObjectPtr &connection, const std::string &host, const TlsClientSpec &tls);
    [[nodiscard]] OpPtr tcp_start_tls_server(const ObjectPtr &connection, const TlsServerSpec &tls);
    void tcp_close(const ObjectPtr &connection);
    [[nodiscard]] bool tcp_is_open(const ObjectPtr &connection);
    /// Fields: "peer", "local", and for TLS "protocol", "cipher", "alpn", "peer_sha256".
    [[nodiscard]] Fields tcp_info(const ObjectPtr &connection);
    [[nodiscard]] OpPtr tcp_set_nodelay(const ObjectPtr &connection, bool enabled);
    [[nodiscard]] OpPtr tcp_set_keepalive(const ObjectPtr &connection, bool enabled);

    // ---------------------------------------------------------------------------------------------------------------
    // UDP  (kind "udp")
    // ---------------------------------------------------------------------------------------------------------------
    struct UdpSpec {
        bool reuse_address = false;
        bool broadcast = false;
        std::uint64_t receive_buffer = 0; ///< bytes; 0: the OS default
        std::uint64_t send_buffer = 0;
    };
    [[nodiscard]] OpPtr udp_bind(const std::string &host, std::uint16_t port, const UdpSpec &spec); ///< object = "udp"
    /// Fixes the peer, so `udp_send` needs no address and only that peer's datagrams are received.
    [[nodiscard]] OpPtr udp_connect(const ObjectPtr &socket, const std::string &host, std::uint16_t port);
    [[nodiscard]] OpPtr udp_send(const ObjectPtr &socket, Bytes data);                                                  ///< on a connected socket; number = bytes sent
    [[nodiscard]] OpPtr udp_send_to(const ObjectPtr &socket, const std::string &host, std::uint16_t port, Bytes data); ///< number = bytes sent
    /// bytes = datagram, text = sender host, number = sender port.
    [[nodiscard]] OpPtr udp_receive(const ObjectPtr &socket, std::size_t max_bytes);
    [[nodiscard]] std::uint16_t udp_port(const ObjectPtr &socket);
    /// "host:port" the socket is bound to.
    [[nodiscard]] std::string udp_address(const ObjectPtr &socket);
    [[nodiscard]] OpPtr udp_set_broadcast(const ObjectPtr &socket, bool enabled);
    /// `interface_address` empty: the default interface.
    [[nodiscard]] OpPtr udp_join_multicast(const ObjectPtr &socket, const std::string &group, const std::string &interface_address);
    [[nodiscard]] OpPtr udp_leave_multicast(const ObjectPtr &socket, const std::string &group, const std::string &interface_address);
    [[nodiscard]] OpPtr udp_set_multicast_loopback(const ObjectPtr &socket, bool enabled);
    [[nodiscard]] OpPtr udp_set_multicast_ttl(const ObjectPtr &socket, std::uint32_t hops);
    void udp_close(const ObjectPtr &socket);
    [[nodiscard]] bool udp_is_open(const ObjectPtr &socket);

    // ---------------------------------------------------------------------------------------------------------------
    // HTTP client  (kind "http_client")
    // ---------------------------------------------------------------------------------------------------------------
    struct HttpClientSpec {
        std::int64_t timeout_ms = 30'000;
        std::uint32_t max_redirects = 5;
        std::uint64_t max_response_body_bytes = 16u << 20;
        bool keep_alive = true;
        bool decompress = true;
        bool cookies = false;
        bool http2 = true;
        bool http3 = true;
        bool http3_prior_knowledge = false;
        std::string user_agent = "SturdyEngine-Async";
        std::string proxy;
        std::vector<std::string> no_proxy;
        TlsClientSpec tls;
        bool allow_https_downgrade = false; ///< follow an https -> http redirect (off: refused)
        std::uint32_t max_response_header_bytes = 64u << 10;
        std::uint32_t max_idle_connections_per_host = 4;
        std::int64_t idle_connection_timeout_ms = 30'000;
        std::int64_t http3_connect_timeout_ms = 3'000;
        ObjectPtr cookie_jar; ///< a "cookie_jar" to share or persist; with `cookies` set and none given, a private one is used
    };
    [[nodiscard]] ObjectPtr http_client_new(const HttpClientSpec &spec);
    /// The client's cookie jar ("cookie_jar"), or null when cookies are off.
    [[nodiscard]] ObjectPtr http_client_cookie_jar(const ObjectPtr &client);
    /// number = status, fields = response headers, bytes = body, text = final URL.
    [[nodiscard]] OpPtr http_request(const ObjectPtr &client, const std::string &method, const std::string &url, const Fields &headers, Bytes body);
    void http_client_close_idle(const ObjectPtr &client);

    // ---------------------------------------------------------------------------------------------------------------
    // HTTP server  (kind "http_server")
    // ---------------------------------------------------------------------------------------------------------------
    struct HttpRequestView {
        std::string method;
        std::string target;
        std::string remote_address;
        std::uint32_t version = 11; ///< 11 = HTTP/1.1, 20 = HTTP/2, 30 = HTTP/3
        Fields headers;
        Bytes body;
        /// Set when the body was spooled to disk (`HttpServerSpec::spool_requests_above`): the temporary file holding it
        /// (`body` is then empty). It is deleted after the response is sent.
        std::string body_file;
        // Filled in by a router (empty for a plain handler):
        std::string path;  ///< percent-decoded path without the query
        std::string query; ///< raw query string without '?'
        Fields params;     ///< `:name` / `*name` captures
    };
    struct HttpResponseSpec {
        std::uint32_t status = 200;
        Fields headers;
        Bytes body;
        bool keep_alive = true;
        /// Serve the body from this file instead of `body` (streamed from disk, never fully loaded), optionally a byte range.
        std::string body_file;
        std::uint64_t body_offset = 0;
        std::int64_t body_length = -1; ///< -1: to the end of the file
        /// Stream the body with chunked transfer encoding: `respond` then returns an "http_body" object to write chunks to.
        bool stream = false;
        /// Server-Sent Events: implies `stream`, adds Content-Type text/event-stream, Cache-Control no-cache and X-Accel-Buffering no.
        bool sse = false;
        std::uint32_t stream_queue = 16; ///< chunks buffered before `http_body_write` waits for the client
    };
    /// Answers the request. Returns the body writer (kind "http_body") when `stream`/`sse` was set, otherwise null.
    using HttpRespond = std::function<ObjectPtr(HttpResponseSpec)>;
    /// Called on a handler thread for each request. Call `respond` exactly once, from any thread, now or later.
    using HttpHandler = std::function<void(HttpRequestView request, HttpRespond respond)>;
    /// A router middleware is a handler too: answer through `respond` *before returning* to stop the request there; return without
    /// answering to let it continue to the route. (Answering later is ignored.)
    using HttpMiddleware = HttpHandler;
    /// A WebSocket upgraded on an HTTP server path: the open socket (kind "websocket") and the upgrade request. Runs on a handler thread.
    using WebSocketHandler = std::function<void(ObjectPtr socket, HttpRequestView request)>;

    struct HttpServerSpec {
        std::string bind_address = "0.0.0.0";
        std::uint16_t port = 0;
        std::string tls_certificate_chain_file;
        std::string tls_private_key_file;
        unsigned io_threads = 0;
        unsigned handler_threads = 0;
        unsigned max_connections = 256;
        std::uint64_t max_request_body_bytes = 1u << 20;
        std::uint32_t max_request_header_bytes = 32u << 10;
        std::uint64_t max_response_body_bytes = 8u << 20;
        std::uint32_t shutdown_timeout_ms = 5000;
        unsigned max_connections_per_ip = 0;
        /// Request bodies larger than this are written to a temporary file (`HttpRequestView::body_file`). 0: always in memory.
        std::uint64_t spool_requests_above = 0;
        std::uint64_t max_spooled_body_bytes = 1ull << 30;
        bool enable_http2 = true;
        unsigned http2_max_concurrent_streams = 100;
        bool enable_http3 = false;
        std::uint16_t http3_port = 0;
        unsigned http3_max_concurrent_streams = 100;
        std::uint64_t http3_max_connections = 1024;
        std::uint64_t http3_max_connections_per_address = 64;
        // WebSockets upgraded through `http_server_on_websocket`:
        std::uint64_t websocket_max_message_bytes = 1u << 20;
        std::uint64_t websocket_max_queued_send_bytes = 4u << 20;
        std::int64_t websocket_idle_timeout_ms = 60'000;
        bool websocket_compression = false;
        std::vector<std::string> websocket_allowed_origins; ///< non-empty: an Origin header must match one exactly
        std::vector<std::string> websocket_subprotocols;
        std::uint64_t max_websockets = 1024;
    };
    /// Creates, binds and starts listening in one step; throws `BindingError` if it cannot.
    [[nodiscard]] ObjectPtr http_server_start(const HttpServerSpec &spec, HttpHandler handler);
    /// Same, serving a router (see below).
    [[nodiscard]] ObjectPtr http_server_start_router(const HttpServerSpec &spec, const ObjectPtr &router);
    /// Creates the server without starting it, so WebSocket paths can be added first. `http_server_begin` then starts it.
    [[nodiscard]] ObjectPtr http_server_create(const HttpServerSpec &spec, HttpHandler handler);
    [[nodiscard]] ObjectPtr http_server_create_router(const HttpServerSpec &spec, const ObjectPtr &router);
    /// Upgrades requests whose path equals `path` to WebSockets (over TLS when the server uses it). Call before `http_server_begin`.
    void http_server_on_websocket(const ObjectPtr &server, const std::string &path, WebSocketHandler handler);
    void http_server_begin(const ObjectPtr &server);
    [[nodiscard]] std::uint16_t http_server_port(const ObjectPtr &server);
    [[nodiscard]] std::uint16_t http_server_http3_port(const ObjectPtr &server); ///< 0 when HTTP/3 is not served
    /// Fields "running", "tls", "http2", "port", "http3_port".
    [[nodiscard]] Fields http_server_info(const ObjectPtr &server);
    [[nodiscard]] Fields http_server_metrics(const ObjectPtr &server);
    void http_server_stop(const ObjectPtr &server);

    // ---- body writer for streamed responses (kind "http_body") ----
    /// Queues a chunk. Finishes when it is accepted into the (bounded) queue; fails once the client is gone or the body is finished.
    [[nodiscard]] OpPtr http_body_write(const ObjectPtr &body, Bytes data);
    struct SseEventSpec {
        std::string data;
        std::string event;
        std::string id;
        std::int64_t retry_ms = -1; ///< -1: none
    };
    /// Queues one Server-Sent Event (line breaks in `event`/`id` are dropped so a hostile value cannot inject fields).
    [[nodiscard]] OpPtr http_body_write_sse(const ObjectPtr &body, const SseEventSpec &event);
    /// Ends the body (the last chunk is sent once queued data drains). Releasing the writer does the same.
    void http_body_finish(const ObjectPtr &body);
    /// Finishes when the client disconnected or the server stopped (so a producer can stop). Cancelled if the op is cancelled.
    [[nodiscard]] OpPtr http_body_closed(const ObjectPtr &body);

    /// The wire format of one event, ending in the blank line that dispatches it.
    [[nodiscard]] std::string format_sse(const SseEventSpec &event);
    /// Percent-decodes `text`; throws BindingError on a malformed escape or an encoded NUL.
    [[nodiscard]] std::string percent_decode(const std::string &text, bool plus_is_space);
    enum class ResponseKind { Text, Html, Json, Redirect };
    /// Canned responses: `Text`/`Html`/`Json` use `body` (JSON must already be valid text); `Redirect` uses `body` as the Location.
    [[nodiscard]] HttpResponseSpec canned_response(ResponseKind kind, std::uint32_t status, const std::string &body);

    // ---- router (kind "router") ----
    struct StaticFilesSpec {
        std::string root;
        std::string index_file = "index.html";
        std::string cache_control = "public, max-age=300";
        bool allow_dotfiles = false;
        bool follow_symlinks = false;
        bool serve_precompressed = true;
        std::string spa_fallback;
        Fields mime_types; ///< extension (".glb") -> MIME type
    };
    /// A URL router: patterns are '/'-separated literals, `:name` (one segment) and a final `*name`. Literals beat parameters beat wildcards.
    /// Build it completely, then serve it: a running server holds an immutable snapshot, so later edits do not affect it.
    [[nodiscard]] ObjectPtr router_new();
    void router_route(const ObjectPtr &router, const std::string &method, const std::string &pattern, HttpHandler handler);
    void router_use(const ObjectPtr &router, HttpMiddleware middleware);
    /// Serves files under `mount` from `spec.root`, GET and HEAD only. Throws if the root is not a directory.
    void router_static_files(const ObjectPtr &router, const std::string &mount, const StaticFilesSpec &spec);
    void router_not_found(const ObjectPtr &router, HttpHandler handler);
    /// Middleware answering 429 + Retry-After once one client address exceeds `rate_per_second` (bursts up to `burst`).
    void router_rate_limit(const ObjectPtr &router, double rate_per_second, double burst, std::uint64_t max_keys);
    /// Runs one request through the router synchronously (tests, in-process calls). Streamed bodies are not collected.
    [[nodiscard]] HttpResponseSpec router_dispatch(const ObjectPtr &router, HttpRequestView request);

    // ---- keyed rate limiter (kind "rate_limiter") ----
    [[nodiscard]] ObjectPtr rate_limiter_new(double rate_per_second, double burst, std::uint64_t max_keys);
    [[nodiscard]] bool rate_limiter_try_acquire(const ObjectPtr &limiter, const std::string &key, double cost);
    [[nodiscard]] std::int64_t rate_limiter_retry_after_ms(const ObjectPtr &limiter, const std::string &key, double cost);
    [[nodiscard]] std::uint64_t rate_limiter_tracked_keys(const ObjectPtr &limiter);

    // ---- cookie jar (kind "cookie_jar") ----
    struct CookieSpec {
        std::string name;
        std::string value;
        std::string domain;
        std::string path = "/";
        bool host_only = true;
        bool secure = false;
        bool http_only = false;
        std::int64_t expires_unix = -1; ///< seconds since the epoch; -1: a session cookie
    };
    [[nodiscard]] ObjectPtr cookie_jar_new();
    void cookie_jar_add(const ObjectPtr &jar, const CookieSpec &cookie);
    void cookie_jar_store_from_response(const ObjectPtr &jar, const std::string &host, const std::string &path, bool https, const Fields &response_headers);
    /// "a=1; b=2" for the cookies that apply to this request (empty if none).
    [[nodiscard]] std::string cookie_jar_header_for(const ObjectPtr &jar, const std::string &host, const std::string &path, bool https);
    [[nodiscard]] std::vector<CookieSpec> cookie_jar_all(const ObjectPtr &jar);
    void cookie_jar_clear(const ObjectPtr &jar);

    // ---------------------------------------------------------------------------------------------------------------
    // WebSocket  (kinds "websocket", "ws_server")
    // ---------------------------------------------------------------------------------------------------------------
    struct WebSocketSpec {
        std::uint64_t max_message_bytes = 1u << 20;
        std::uint64_t max_queued_send_bytes = 4u << 20;
        std::int64_t idle_timeout_ms = 60'000;
        bool compression = false;
        std::uint64_t inbox_messages = 16; ///< received messages buffered before the reader is paused
        std::int64_t handshake_timeout_ms = 10'000;
    };
    struct WebSocketClientSpec {
        WebSocketSpec socket;
        TlsClientSpec tls;
        Fields headers;
        std::vector<std::string> subprotocols;
        std::int64_t timeout_ms = 0;
    };
    struct WebSocketServerSpec {
        std::string bind_address;
        std::uint16_t port = 0;
        WebSocketSpec socket;
        bool use_tls = false;
        TlsServerSpec tls;
        std::vector<std::string> allowed_origins;
        std::vector<std::string> paths;
        std::vector<std::string> subprotocols;
        std::uint64_t max_connections = 1024;
        std::uint32_t max_request_header_bytes = 16u << 10;
    };
    [[nodiscard]] OpPtr websocket_connect(const std::string &url, const WebSocketClientSpec &spec); ///< object = "websocket"
    [[nodiscard]] OpPtr websocket_server_listen(const WebSocketServerSpec &spec);                     ///< object = "ws_server"
    [[nodiscard]] OpPtr websocket_accept(const ObjectPtr &server);                                    ///< object = "websocket"
    [[nodiscard]] std::uint16_t websocket_server_port(const ObjectPtr &server);
    void websocket_server_close(const ObjectPtr &server);
    [[nodiscard]] OpPtr websocket_send_text(const ObjectPtr &socket, std::string text);
    [[nodiscard]] OpPtr websocket_send_binary(const ObjectPtr &socket, Bytes data);
    [[nodiscard]] OpPtr websocket_ping(const ObjectPtr &socket, Bytes payload);
    /// bytes = payload, number = 1 for a text message and 0 for binary.
    [[nodiscard]] OpPtr websocket_receive(const ObjectPtr &socket);
    [[nodiscard]] OpPtr websocket_close(const ObjectPtr &socket, std::uint16_t code, std::string reason);
    [[nodiscard]] bool websocket_is_open(const ObjectPtr &socket);
    /// Drops the connection without a closing handshake.
    void websocket_abort(const ObjectPtr &socket);
    /// Fields: "target", "origin", "subprotocol", "header:<Name>" per upgrade-request header, "peer" (host:port), the TLS fields
    /// (over wss), and once closed "close_code", "close_reason".
    [[nodiscard]] Fields websocket_info(const ObjectPtr &socket);

    // ---------------------------------------------------------------------------------------------------------------
    // QUIC  (kinds "quic_conn", "quic_stream", "quic_listener")
    // ---------------------------------------------------------------------------------------------------------------
    struct QuicLimitsSpec {
        std::int64_t idle_timeout_ms = 30'000;
        std::int64_t keep_alive_ms = 0;
        std::int64_t handshake_timeout_ms = 10'000;
        std::uint32_t max_bidirectional_streams = 100;
        std::uint32_t max_unidirectional_streams = 100;
        std::uint64_t stream_receive_window = 256u << 10;
        std::uint64_t connection_receive_window = 2u << 20;
        bool datagrams = true;
        std::uint64_t stream_send_buffer = 1u << 20;
        std::uint64_t datagram_receive_queue = 256; ///< datagrams buffered for `quic_receive_datagram`
        std::uint64_t datagram_send_queue = 256;
    };
    struct QuicClientSpec {
        QuicLimitsSpec limits;
        TlsClientSpec tls; ///< `alpn` is required
    };
    struct QuicServerSpec {
        QuicLimitsSpec limits;
        TlsServerSpec tls; ///< `alpn` is required
        std::string bind_address;
        std::uint16_t port = 0;
        std::uint64_t max_connections = 1024;
        std::uint64_t max_connections_per_address = 32;
        bool require_address_validation = true;
    };
    [[nodiscard]] OpPtr quic_connect(const std::string &host, std::uint16_t port, const QuicClientSpec &spec); ///< object = "quic_conn"
    [[nodiscard]] OpPtr quic_listen(const QuicServerSpec &spec);                                                ///< object = "quic_listener"
    [[nodiscard]] OpPtr quic_accept(const ObjectPtr &listener);                                                 ///< object = "quic_conn"
    [[nodiscard]] std::uint16_t quic_listener_port(const ObjectPtr &listener);
    [[nodiscard]] Fields quic_listener_metrics(const ObjectPtr &listener);
    void quic_listener_stop(const ObjectPtr &listener);
    [[nodiscard]] OpPtr quic_open_stream(const ObjectPtr &connection, bool bidirectional);  ///< object = "quic_stream"
    [[nodiscard]] OpPtr quic_accept_stream(const ObjectPtr &connection);                    ///< object = "quic_stream"
    /// Throws BindingError when refused (too big, queue full, peer takes none).
    void quic_send_datagram(const ObjectPtr &connection, Bytes data);
    [[nodiscard]] OpPtr quic_receive_datagram(const ObjectPtr &connection);                 ///< bytes
    [[nodiscard]] std::size_t quic_max_datagram_size(const ObjectPtr &connection);
    void quic_close(const ObjectPtr &connection, std::uint64_t code, std::string reason);
    /// Finishes when the connection ends: number = code, text = reason, fields "from_peer", "application", "timed_out".
    [[nodiscard]] OpPtr quic_closed(const ObjectPtr &connection);
    [[nodiscard]] bool quic_is_open(const ObjectPtr &connection);
    /// Fields "peer", "local", "protocol", "alpn", "peer_sha256" (the TLS ones once the handshake finished).
    [[nodiscard]] Fields quic_info(const ObjectPtr &connection);
    /// Fields "rtt_us", "min_rtt_us", "bytes_in_flight", "cwnd", "packets_lost", "bytes_sent", "bytes_received".
    [[nodiscard]] Fields quic_stats(const ObjectPtr &connection);
    [[nodiscard]] OpPtr quic_stream_send(const ObjectPtr &stream, Bytes data);
    [[nodiscard]] OpPtr quic_stream_finish(const ObjectPtr &stream);
    [[nodiscard]] OpPtr quic_stream_receive(const ObjectPtr &stream, std::size_t max_bytes); ///< bytes (empty = peer finished)
    void quic_stream_reset(const ObjectPtr &stream, std::uint64_t code);
    void quic_stream_stop_sending(const ObjectPtr &stream, std::uint64_t code);
    [[nodiscard]] std::int64_t quic_stream_id(const ObjectPtr &stream);
    [[nodiscard]] bool quic_stream_is_bidirectional(const ObjectPtr &stream);

    // ---------------------------------------------------------------------------------------------------------------
    // Peer-to-peer, relay, NAT traversal  (kinds "peer", "relay_server", "mapping")
    // ---------------------------------------------------------------------------------------------------------------
    struct PeerSpec {
        std::string network_id;
        std::string display_name;
        bool has_identity = false;
        Identity identity;
        bool listen = true;
        std::string bind_address;
        std::uint16_t port = 0;
        std::uint64_t max_peers = 64;
        std::uint64_t max_message_bytes = 1u << 20;
        std::vector<std::string> allowed_peers;
        bool peer_exchange = true;
        bool lan_discovery = false;
        bool auto_connect = true;
        std::int64_t handshake_timeout_ms = 10'000;
        std::uint64_t max_pending_inbound = 32;
        std::int64_t ping_interval_ms = 15'000;
        std::int64_t idle_timeout_ms = 45'000;
        std::uint64_t event_queue = 1024;
        std::uint16_t discovery_port = 47831;
        std::string discovery_group = "239.255.83.31";
        std::int64_t discovery_interval_ms = 5'000;
    };
    [[nodiscard]] OpPtr peer_start(const PeerSpec &spec); ///< object = "peer", text = our id
    [[nodiscard]] std::string peer_id(const ObjectPtr &peer);
    [[nodiscard]] std::uint16_t peer_listen_port(const ObjectPtr &peer);
    [[nodiscard]] Identity peer_identity(const ObjectPtr &peer);
    /// fields = "id","name","address","inbound","listen_port" of the connected peer.
    [[nodiscard]] OpPtr peer_connect(const ObjectPtr &peer, const std::string &host, std::uint16_t port, const std::string &expected_id);
    /// `relay_tls` non-null: the hop to the relay itself uses TLS (the peers' own end-to-end TLS runs inside it either way).
    [[nodiscard]] OpPtr peer_connect_via_relay(const ObjectPtr &peer, const std::string &relay_host, std::uint16_t relay_port, const std::string &token,
                                               bool dial, const std::string &expected_id, const TlsClientSpec *relay_tls);
    [[nodiscard]] OpPtr peer_send(const ObjectPtr &peer, const std::string &target_id, std::uint16_t channel, Bytes data);
    /// number = links that took it. `except_id` non-empty: that peer is skipped (forwarding what it just sent).
    [[nodiscard]] OpPtr peer_broadcast(const ObjectPtr &peer, std::uint16_t channel, Bytes data, const std::string &except_id);
    /// number = event kind (0 connected, 1 disconnected, 2 message, 3 discovered); bytes = message; fields "peer","channel",
    /// "name","address","reason","inbound".
    [[nodiscard]] OpPtr peer_next_event(const ObjectPtr &peer);
    /// One entry per connected peer: fields "id:<n>", "name:<n>", "address:<n>", "inbound:<n>", "listen_port:<n>".
    [[nodiscard]] Fields peer_list(const ObjectPtr &peer);
    /// `reason` empty: "disconnected by application".
    void peer_disconnect(const ObjectPtr &peer, const std::string &id, const std::string &reason);
    void peer_block(const ObjectPtr &peer, const std::string &id);
    void peer_stop(const ObjectPtr &peer);

    struct RelaySpec {
        std::string bind_address;
        std::uint16_t port = 0;
        bool use_tls = false;
        TlsServerSpec tls;
        std::uint64_t max_sessions = 1024;
        std::uint64_t max_waiting = 1024;
        std::uint64_t max_waiting_per_address = 16;
        std::int64_t pair_timeout_ms = 30'000;
        std::int64_t idle_timeout_ms = 120'000;
        std::uint64_t max_session_bytes = 0;
        std::uint64_t max_token_bytes = 128;
    };
    /// Lets the relay authenticate: called (on a network thread -- answer quickly) with the rendezvous token and the client's address;
    /// returning false refuses the client.
    using RelayAuthorizeFn = std::function<bool(const std::string &token, const std::string &client_address)>;
    [[nodiscard]] ObjectPtr relay_server_start(const RelaySpec &spec, RelayAuthorizeFn authorize);
    [[nodiscard]] std::uint16_t relay_server_port(const ObjectPtr &relay);
    [[nodiscard]] Fields relay_server_metrics(const ObjectPtr &relay);
    void relay_server_stop(const ObjectPtr &relay);
    [[nodiscard]] std::string random_relay_token();
    /// object = "tcp" connected to the partner (start TLS on it before sending anything private).
    [[nodiscard]] OpPtr relay_connect(const std::string &host, std::uint16_t port, const std::string &token, bool dial, const TlsClientSpec *relay_tls,
                                      std::int64_t timeout_ms);

    struct PortMapSpec {
        std::string gateway;
        std::int64_t lifetime_s = 3600;
        std::uint16_t external_port = 0;
        std::int64_t timeout_ms = 3000;
        std::uint32_t methods = 7; ///< bit 0 PCP, bit 1 NAT-PMP, bit 2 UPnP
        std::uint16_t gateway_port = 5351;
        std::string ssdp_host = "239.255.255.250";
        std::uint16_t ssdp_port = 1900;
    };
    /// object = "mapping"; number = granted lifetime in seconds (0: permanent); fields "external_host","external_port","method","gateway".
    [[nodiscard]] OpPtr map_port(bool tcp, std::uint16_t internal_port, const PortMapSpec &spec);
    [[nodiscard]] OpPtr unmap_port(const ObjectPtr &mapping, const PortMapSpec &spec);
    [[nodiscard]] std::string default_gateway(); ///< empty when unknown
    /// text = confirmed peer host, number = its port.
    [[nodiscard]] OpPtr punch_udp_hole(const ObjectPtr &socket, const std::vector<std::pair<std::string, std::uint16_t>> &candidates, const std::string &secret,
                                       std::int64_t timeout_ms);

    // ---------------------------------------------------------------------------------------------------------------
    // Files, watching, processes
    // ---------------------------------------------------------------------------------------------------------------
    enum FileModeBits : std::uint32_t { FileRead = 1, FileWrite = 2, FileCreate = 4, FileTruncate = 8, FileAppend = 16, FileCreateNew = 32 };
    [[nodiscard]] OpPtr file_open(const std::string &path, std::uint32_t mode); ///< object = "file"
    [[nodiscard]] OpPtr file_read(const ObjectPtr &file, std::size_t max_bytes);
    [[nodiscard]] OpPtr file_read_at(const ObjectPtr &file, std::uint64_t offset, std::size_t max_bytes);
    [[nodiscard]] OpPtr file_read_to_end(const ObjectPtr &file, std::size_t max_bytes);
    [[nodiscard]] OpPtr file_write(const ObjectPtr &file, Bytes data);
    [[nodiscard]] OpPtr file_write_at(const ObjectPtr &file, std::uint64_t offset, Bytes data);
    [[nodiscard]] OpPtr file_seek(const ObjectPtr &file, std::int64_t offset, int origin); ///< origin 0 begin, 1 current, 2 end; number = position
    [[nodiscard]] OpPtr file_size(const ObjectPtr &file);                                   ///< number
    [[nodiscard]] OpPtr file_set_len(const ObjectPtr &file, std::uint64_t length);
    [[nodiscard]] OpPtr file_sync(const ObjectPtr &file, bool data_only);
    void file_close(const ObjectPtr &file);

    struct WatchSpec {
        bool recursive = false;
        std::int64_t debounce_ms = 50;
        bool force_polling = false;
        std::int64_t poll_interval_ms = 250; ///< the polling backend's scan period
        std::uint64_t max_pending_events = 4096;
    };
    /// Throws BindingError when the path cannot be watched.
    [[nodiscard]] ObjectPtr watch_path(const std::string &path, const WatchSpec &spec); ///< kind "watcher"
    /// number = kind (0 created, 1 modified, 2 removed, 3 renamed, 4 overflow); text = path; fields "old_path","is_directory".
    [[nodiscard]] OpPtr watch_next(const ObjectPtr &watcher);
    /// True when the OS's event API is used (inotify), false for the polling fallback.
    [[nodiscard]] bool watch_uses_native_backend(const ObjectPtr &watcher);
    /// The remaining events as a stream; each item is `pack_frames({kind as decimal text, path, old_path, "1"/"0" for is_directory})`.
    [[nodiscard]] ObjectPtr watch_events_stream(const ObjectPtr &watcher);
    void watch_close(const ObjectPtr &watcher);

    // ---- whole-file helpers and directories (run on the IO lane) ----
    /// bytes = the file; Failed(LimitExceeded) past `max_bytes` (0: unlimited).
    [[nodiscard]] OpPtr fs_read_file(const std::string &path, std::uint64_t max_bytes);
    [[nodiscard]] OpPtr fs_read_range(const std::string &path, std::uint64_t offset, std::uint64_t byte_count);
    /// Creates or replaces the file (or appends to it).
    [[nodiscard]] OpPtr fs_write_file(const std::string &path, Bytes data, bool append);
    [[nodiscard]] OpPtr fs_size(const std::string &path);               ///< number
    [[nodiscard]] OpPtr fs_exists(const std::string &path);             ///< number = 1 / 0
    [[nodiscard]] OpPtr fs_create_directories(const std::string &path);
    [[nodiscard]] OpPtr fs_remove_file(const std::string &path);        ///< number = 1 if something was removed
    [[nodiscard]] OpPtr fs_remove_directory(const std::string &path);   ///< number = 1 if something was removed
    [[nodiscard]] OpPtr fs_copy(const std::string &source, const std::string &destination);
    [[nodiscard]] OpPtr fs_rename(const std::string &source, const std::string &destination);
    /// bytes = `pack_frames` of the entry paths, number = how many. At most `max_entries` (0: 4096).
    [[nodiscard]] OpPtr fs_list(const std::string &path, std::uint64_t max_entries, bool recursive);
    /// Walks a directory entry by entry (kind "dir_iter"): `dir_iter_next` gives number = 1 + text = path, or number = 0 at the end.
    [[nodiscard]] ObjectPtr dir_iter_new(const std::string &path, bool recursive);
    [[nodiscard]] OpPtr dir_iter_next(const ObjectPtr &iterator);
    [[nodiscard]] bool file_is_open(const ObjectPtr &file);

    enum class StdioKind : std::uint32_t { Pipe = 0, Null = 1, Inherit = 2 };
    struct ProcessSpec {
        std::string program;
        std::vector<std::string> args;
        std::string working_directory;
        Fields environment;
        bool clear_environment = false;
        bool kill_on_drop = false;
        // For `process_spawn` (a `process_run` always pipes what it collects). A stream that is not a pipe cannot be read or written.
        StdioKind stdin_mode = StdioKind::Pipe;
        StdioKind stdout_mode = StdioKind::Pipe;
        StdioKind stderr_mode = StdioKind::Pipe;
    };
    /// Runs to completion. bytes = stdout, text = stderr, number = exit code (signal number when signalled), fields "signaled","success".
    [[nodiscard]] OpPtr process_run(const ProcessSpec &spec, Bytes stdin_data, std::int64_t timeout_ms, std::uint64_t max_output_bytes);
    /// Starts a child with piped stdin/stdout/stderr; throws BindingError if it cannot start. Kind "process".
    [[nodiscard]] ObjectPtr process_spawn(const ProcessSpec &spec);
    [[nodiscard]] OpPtr process_write_stdin(const ObjectPtr &process, Bytes data);
    void process_close_stdin(const ObjectPtr &process);
    [[nodiscard]] OpPtr process_read_stdout(const ObjectPtr &process, std::size_t max_bytes);
    [[nodiscard]] OpPtr process_read_stderr(const ObjectPtr &process, std::size_t max_bytes);
    [[nodiscard]] OpPtr process_wait(const ObjectPtr &process); ///< number = exit code, fields "signaled","success","signal"
    [[nodiscard]] std::int64_t process_id(const ObjectPtr &process);
    /// Asks the child to exit (SIGTERM / a close request); `process_kill` forces it.
    void process_request_exit(const ObjectPtr &process);
    void process_kill(const ObjectPtr &process);

    // ---------------------------------------------------------------------------------------------------------------
    // Generic streams: copy, buffer and frame between any of tcp, file, process pipes, memory and pipe ends
    // ---------------------------------------------------------------------------------------------------------------
    // Readers: "tcp", "file", "pipe_end", "memory_reader", "process" (stdout or stderr). Writers: "tcp", "file", "pipe_end", "memory_writer",
    // "process" (stdin). `ProcessStream` says which pipe of a "process" is meant and is ignored for every other kind.
    enum class ProcessStream : std::uint32_t { Stdout = 0, Stderr = 1, Stdin = 2 };
    struct CopySpec {
        std::uint64_t chunk_bytes = 64 * 1024;
        std::uint64_t limit = 0; ///< 0: until the reader ends
        bool shutdown_writer = false;
    };
    /// Copies until the reader ends (or `limit` bytes). number = bytes copied.
    [[nodiscard]] OpPtr io_copy(const ObjectPtr &reader, ProcessStream reader_stream, const ObjectPtr &writer, ProcessStream writer_stream, const CopySpec &spec);
    /// bytes = everything until the reader ends; Failed(LimitExceeded) past `max_bytes` (0: unlimited).
    [[nodiscard]] OpPtr io_read_to_end(const ObjectPtr &reader, ProcessStream stream, std::uint64_t max_bytes);
    [[nodiscard]] OpPtr io_write_all(const ObjectPtr &writer, ProcessStream stream, Bytes data);

    [[nodiscard]] ObjectPtr memory_reader_new(Bytes data);
    [[nodiscard]] ObjectPtr memory_writer_new();
    [[nodiscard]] Bytes memory_writer_bytes(const ObjectPtr &writer);
    /// Two connected in-memory pipe ends ("pipe_end"): what one writes the other reads. `chunks`: messages buffered per direction.
    [[nodiscard]] ChannelEnds memory_pipe_new(std::uint64_t chunks);

    /// Adds read-ahead to a reader: lines, delimiters, exact counts. One operation at a time per object.
    [[nodiscard]] ObjectPtr buffered_reader_new(const ObjectPtr &reader, ProcessStream stream, std::uint64_t chunk_bytes);
    [[nodiscard]] OpPtr buffered_read_some(const ObjectPtr &reader, std::uint64_t max_bytes);   ///< bytes (empty: end)
    [[nodiscard]] OpPtr buffered_read_exact(const ObjectPtr &reader, std::uint64_t count);      ///< bytes; Failed(UnexpectedEof) when short
    /// number = 1 and bytes = the record including the delimiter, or number = 0 at a clean end of stream.
    [[nodiscard]] OpPtr buffered_read_until(const ObjectPtr &reader, const std::string &delimiter, std::uint64_t max_bytes);
    /// number = 1 and text = the line without its terminator, or number = 0 at a clean end of stream.
    [[nodiscard]] OpPtr buffered_read_line(const ObjectPtr &reader, std::uint64_t max_bytes);
    [[nodiscard]] std::uint64_t buffered_reader_buffered(const ObjectPtr &reader);
    /// Batches small writes. Call `buffered_flush` (or `buffered_shutdown`, which also shuts the writer down) to send what is pending.
    [[nodiscard]] ObjectPtr buffered_writer_new(const ObjectPtr &writer, ProcessStream stream, std::uint64_t capacity);
    [[nodiscard]] OpPtr buffered_write(const ObjectPtr &writer, Bytes data);
    [[nodiscard]] OpPtr buffered_flush(const ObjectPtr &writer);
    [[nodiscard]] OpPtr buffered_shutdown(const ObjectPtr &writer);

    enum class FrameCodec : std::uint32_t { LengthDelimited = 0, Lines = 1 };
    /// Messages over a byte stream ("framed"): a 4-byte big-endian length prefix, or one line each. `io` must read and write
    /// ("tcp", "file", "pipe_end"). `max_frame_bytes` 0: 8 MiB (64 KiB for lines).
    [[nodiscard]] ObjectPtr framed_new(const ObjectPtr &io, FrameCodec codec, std::uint64_t max_frame_bytes);
    /// number = 1 and bytes = the frame, or number = 0 at a clean end of stream.
    [[nodiscard]] OpPtr framed_next(const ObjectPtr &framed);
    [[nodiscard]] OpPtr framed_send(const ObjectPtr &framed, Bytes frame);
    /// The remaining frames as a stream (the framed object must not be used for `framed_next` meanwhile).
    [[nodiscard]] ObjectPtr framed_stream(const ObjectPtr &framed);

    // ---------------------------------------------------------------------------------------------------------------
    // Data parallelism (the Rayon half) and thread pools  (kind "pool")
    // ---------------------------------------------------------------------------------------------------------------
    /// Runs `body(i)` for every i in [0, count) across the worker pool and returns when all are done. Exceptions propagate.
    void par_for(std::uint64_t count, std::uint64_t min_chunk, const std::function<void(std::uint64_t)> &body);
    /// Runs `body(begin, end)` over consecutive ranges of at most `chunk` indices.
    void par_for_chunks(std::uint64_t count, std::uint64_t chunk, const std::function<void(std::uint64_t, std::uint64_t)> &body);
    enum class Numeric { F32, F64, I32, I64, U32, U64 };
    /// In-place parallel sort (ascending).
    void par_sort(Numeric type, void *data, std::uint64_t count);
    [[nodiscard]] double par_sum_f64(const double *data, std::uint64_t count);
    [[nodiscard]] float par_sum_f32(const float *data, std::uint64_t count);
    [[nodiscard]] std::int64_t par_sum_i64(const std::int64_t *data, std::uint64_t count);
    [[nodiscard]] std::uint64_t par_sum_u64(const std::uint64_t *data, std::uint64_t count);
    /// Min / max; false when `count` is 0. Float results ignore NaN ordering guarantees (NaN-free input expected).
    [[nodiscard]] bool par_min_max_f64(const double *data, std::uint64_t count, double *min, double *max);
    [[nodiscard]] bool par_min_max_f32(const float *data, std::uint64_t count, float *min, float *max);
    [[nodiscard]] bool par_min_max_i64(const std::int64_t *data, std::uint64_t count, std::int64_t *min, std::int64_t *max);

    [[nodiscard]] ObjectPtr pool_create(std::uint32_t threads, const std::string &name);
    [[nodiscard]] std::uint32_t pool_threads(const ObjectPtr &pool);
    /// Like `par_for`, but every task runs on the pool's own threads.
    void pool_par_for(const ObjectPtr &pool, std::uint64_t count, std::uint64_t min_chunk, const std::function<void(std::uint64_t)> &body);
    /// Runs `body()` on the pool and waits for it.
    void pool_install(const ObjectPtr &pool, const std::function<void()> &body);

    // ---------------------------------------------------------------------------------------------------------------
    // Cancellation  (kinds "cancel_source", "cancel_token")
    // ---------------------------------------------------------------------------------------------------------------
    [[nodiscard]] ObjectPtr cancel_source_new();
    /// Cancels the source and runs everything armed on it. True only for the first call.
    bool cancel_source_cancel(const ObjectPtr &source);
    [[nodiscard]] bool cancel_source_is_cancelled(const ObjectPtr &source);
    /// A token observing the source (cheap; as many as you like).
    [[nodiscard]] ObjectPtr cancel_source_token(const ObjectPtr &source);
    [[nodiscard]] bool cancel_token_is_cancelled(const ObjectPtr &token);
    /// Finishes (Ok) when the token is cancelled; finishes at once if it already is.
    [[nodiscard]] OpPtr cancel_token_wait(const ObjectPtr &token);
    /// Cancelling `token` cancels `op` (the link is dropped when `op` finishes).
    void op_link_cancel(const OpPtr &op, const ObjectPtr &token);
    /// A new op that finishes as `op` does, or as Failed(TimedOut) after `timeout_ms` -- cancelling `op` then.
    [[nodiscard]] OpPtr op_with_timeout(const OpPtr &op, std::int64_t timeout_ms);

    // ---------------------------------------------------------------------------------------------------------------
    // Message channels and synchronisation primitives (the Tokio `sync` module). Messages are byte strings.
    // ---------------------------------------------------------------------------------------------------------------
    enum class TryStatus { Ok = 0, Full = 1, Empty = 1, Closed = 2 };

    /// Bounded multi-producer channel ("channel_sender" / "channel_receiver"); `capacity` messages are buffered (at least 1). It closes
    /// when the last sender is released or `channel_close` is called on either end; buffered messages are still delivered.
    [[nodiscard]] ChannelEnds channel_create(std::uint64_t capacity);
    [[nodiscard]] ObjectPtr channel_sender_clone(const ObjectPtr &sender);
    [[nodiscard]] ObjectPtr channel_receiver_clone(const ObjectPtr &receiver);
    /// Finishes once the message is buffered or handed to a receiver (waits while the channel is full); Failed(Closed) if closed.
    [[nodiscard]] OpPtr channel_send(const ObjectPtr &sender, Bytes data);
    [[nodiscard]] TryStatus channel_try_send(const ObjectPtr &sender, Bytes data);
    /// number = 1 and bytes = the message, or number = 0 once the channel is closed and drained.
    [[nodiscard]] OpPtr channel_recv(const ObjectPtr &receiver);
    [[nodiscard]] TryStatus channel_try_recv(const ObjectPtr &receiver, Bytes *out);
    void channel_close(const ObjectPtr &end);

    /// One-shot value hand-off ("oneshot_sender" / "oneshot_receiver"). A sender released without sending fails the receiver with Closed.
    [[nodiscard]] ChannelEnds oneshot_create();
    /// True when this call set the value (false if already set).
    bool oneshot_send(const ObjectPtr &sender, Bytes data);
    bool oneshot_fail(const ObjectPtr &sender, const std::string &message);
    /// bytes = the value. The receiver can be waited on once.
    [[nodiscard]] OpPtr oneshot_recv(const ObjectPtr &receiver);

    /// A single value many tasks observe ("watch_sender" / "watch_receiver"). Receivers track what they have seen independently.
    [[nodiscard]] ChannelEnds watch_create(Bytes initial);
    void watch_send(const ObjectPtr &sender, Bytes data);
    [[nodiscard]] ObjectPtr watch_subscribe(const ObjectPtr &sender);
    [[nodiscard]] ObjectPtr watch_receiver_clone(const ObjectPtr &receiver);
    [[nodiscard]] Bytes watch_borrow(const ObjectPtr &receiver);
    [[nodiscard]] Bytes watch_borrow_and_update(const ObjectPtr &receiver);
    [[nodiscard]] bool watch_has_changed(const ObjectPtr &receiver);
    /// Finishes when the value changed since this receiver last looked; Failed(Closed) once the sender is gone and nothing new remains.
    [[nodiscard]] OpPtr watch_changed(const ObjectPtr &receiver);

    /// Every receiver sees every message ("broadcast_sender" / "broadcast_receiver"); one that falls `capacity` behind is told how many it missed.
    [[nodiscard]] ChannelEnds broadcast_create(std::uint64_t capacity);
    /// Returns the number of live receivers.
    std::uint64_t broadcast_send(const ObjectPtr &sender, Bytes data);
    [[nodiscard]] ObjectPtr broadcast_subscribe(const ObjectPtr &sender);
    [[nodiscard]] ObjectPtr broadcast_receiver_clone(const ObjectPtr &receiver);
    /// number = 0 for a message (bytes), 1 when this receiver lagged (field "skipped"), 2 once the sender is gone. Cannot be cancelled.
    [[nodiscard]] OpPtr broadcast_recv(const ObjectPtr &receiver);

    /// Counting semaphore ("semaphore"); permits are held by "permit" objects and returned when those are released.
    [[nodiscard]] ObjectPtr semaphore_new(std::uint64_t permits);
    /// object = "permit". Failed(Closed) if the semaphore closes first.
    [[nodiscard]] OpPtr semaphore_acquire(const ObjectPtr &semaphore, std::uint64_t count);
    [[nodiscard]] ObjectPtr semaphore_try_acquire(const ObjectPtr &semaphore, std::uint64_t count); ///< null when not available
    void semaphore_add_permits(const ObjectPtr &semaphore, std::uint64_t count);
    void semaphore_close(const ObjectPtr &semaphore);
    [[nodiscard]] std::uint64_t semaphore_available(const ObjectPtr &semaphore);
    [[nodiscard]] bool semaphore_is_closed(const ObjectPtr &semaphore);
    void permit_release(const ObjectPtr &permit);
    /// Keeps the permits consumed (they are never returned).
    void permit_forget(const ObjectPtr &permit);
    [[nodiscard]] std::uint64_t permit_count(const ObjectPtr &permit);

    /// Async mutex / read-write lock protecting a byte-string value; waiting costs no thread. Guards ("mutex_guard", "rwlock_read_guard",
    /// "rwlock_write_guard") unlock when released.
    [[nodiscard]] ObjectPtr async_mutex_new(Bytes initial);
    [[nodiscard]] OpPtr async_mutex_lock(const ObjectPtr &mutex);        ///< object = "mutex_guard"
    [[nodiscard]] ObjectPtr async_mutex_try_lock(const ObjectPtr &mutex); ///< null when held
    [[nodiscard]] ObjectPtr async_rwlock_new(Bytes initial);
    [[nodiscard]] OpPtr async_rwlock_read(const ObjectPtr &lock);  ///< object = "rwlock_read_guard"
    [[nodiscard]] OpPtr async_rwlock_write(const ObjectPtr &lock); ///< object = "rwlock_write_guard"
    [[nodiscard]] Bytes guard_get(const ObjectPtr &guard);
    /// Mutex and write guards only.
    void guard_set(const ObjectPtr &guard, Bytes data);
    void guard_release(const ObjectPtr &guard);

    /// Wake-up signal with one stored permit ("notify").
    [[nodiscard]] ObjectPtr notify_new();
    [[nodiscard]] OpPtr notify_notified(const ObjectPtr &notify);
    void notify_one(const ObjectPtr &notify);
    void notify_waiters(const ObjectPtr &notify);
    /// Rendezvous of `parties` callers ("barrier"); number = 1 for exactly one of them (the last to arrive).
    [[nodiscard]] ObjectPtr barrier_new(std::uint64_t parties);
    [[nodiscard]] OpPtr barrier_wait(const ObjectPtr &barrier);

    // ---------------------------------------------------------------------------------------------------------------
    // Tasks: spawn, join sets, local sets, dedicated threads, task-locals, the main thread
    // ---------------------------------------------------------------------------------------------------------------
    struct TaskOutput {
        std::int64_t number = 0;
        Bytes bytes;
        std::string text;
        std::string error; ///< non-empty: the task failed with this message
    };
    /// The work of a task. `cancel` is the task's token (poll `cancel_token_is_cancelled` to stop early). Exceptions fail the task.
    using TaskFn = std::function<TaskOutput(ObjectPtr cancel)>;
    enum class Lane { Light, Heavy, Blocking };
    /// Runs `fn` on the worker pool (`Light`/`Heavy` scheduler lanes) or the blocking-IO lane. The op carries number/bytes/text; a task
    /// error fails it with Unknown. Cancelling the op sets the token passed to `fn`.
    [[nodiscard]] OpPtr task_spawn(TaskFn fn, Lane lane);
    /// Same, on a custom pool (kind "pool").
    [[nodiscard]] OpPtr task_spawn_on(const ObjectPtr &pool, TaskFn fn);

    /// A dynamic set of tasks joined in completion order ("join_set").
    [[nodiscard]] ObjectPtr join_set_new();
    void join_set_spawn(const ObjectPtr &set, TaskFn fn, Lane lane);
    /// Finishes with the next task to complete. Fields: "outcome" = "ok" | "failed" | "empty" (nothing left) and, for a failure, "error";
    /// number/bytes/text as the task returned them.
    [[nodiscard]] OpPtr join_set_join_next(const ObjectPtr &set);
    [[nodiscard]] std::uint64_t join_set_len(const ObjectPtr &set);
    /// Sets the cancel token of every running task.
    void join_set_abort_all(const ObjectPtr &set);

    /// Tasks that run only on the thread driving the set ("local_set"): the caller pumps them.
    [[nodiscard]] ObjectPtr local_set_new();
    [[nodiscard]] OpPtr local_set_spawn(const ObjectPtr &set, TaskFn fn);
    /// Runs queued tasks on the calling thread until none are left; returns how many ran.
    std::uint64_t local_set_run_pending(const ObjectPtr &set);
    /// Runs queued tasks on the calling thread until `op` finishes (blocks).
    void local_set_block_on(const ObjectPtr &set, const OpPtr &op);

    /// A named thread running submitted work one at a time, in order ("dedicated_thread").
    [[nodiscard]] ObjectPtr dedicated_thread_new(const std::string &name);
    [[nodiscard]] OpPtr dedicated_thread_run(const ObjectPtr &thread, TaskFn fn);
    bool dedicated_thread_pin_to_core(const ObjectPtr &thread, std::uint32_t core_index);
    bool dedicated_thread_pin_to_fastest_core(const ObjectPtr &thread);

    /// Values that follow the flow of work: tasks spawned while a value is bound inherit it ("task_local").
    [[nodiscard]] ObjectPtr task_local_new();
    void task_local_scope(const ObjectPtr &local, Bytes value, const std::function<void()> &body);
    /// False when no value is bound here.
    [[nodiscard]] bool task_local_get(const ObjectPtr &local, Bytes *out);

    /// Work for the engine's main thread, run when it pumps.
    void main_thread_run(std::function<void()> fn);
    void main_thread_pump();
    void main_thread_set_wake_hook(std::function<void()> hook);

    // ---------------------------------------------------------------------------------------------------------------
    // Runtime, topology, timers, signals
    // ---------------------------------------------------------------------------------------------------------------
    /// The scheduler is normally started by the engine; standalone programs may start it (0 workers: one per hardware thread).
    void scheduler_initialize(std::uint32_t workers);
    /// Stops the worker pool. The engine owns it: only a standalone program should call this.
    void scheduler_shutdown();
    [[nodiscard]] bool scheduler_is_running();
    [[nodiscard]] std::uint32_t scheduler_worker_count();
    [[nodiscard]] bool scheduler_on_worker_thread();
    /// Tunes the blocking-IO lane (0 workers: automatic). False if it already started with other settings.
    [[nodiscard]] bool io_lane_configure(std::uint32_t workers, std::uint32_t max_pending);
    /// Fields "backend", "workers", "pending", "capacity".
    [[nodiscard]] Fields io_lane_stats();
    /// Number of network reactor threads; false once the reactor is running.
    [[nodiscard]] bool network_reactor_configure(std::uint32_t threads);
    [[nodiscard]] std::vector<std::uint32_t> ranked_logical_cores();
    [[nodiscard]] std::vector<std::uint32_t> ranked_physical_cores();
    bool pin_current_thread_to_core(std::uint32_t core_index);

    /// Finishes at an absolute wall-clock time (milliseconds since the Unix epoch), or at once if it has passed.
    [[nodiscard]] OpPtr sleep_until_unix_ms(std::int64_t epoch_ms);
    /// A periodic timer ("interval") that does not drift; ticks missed while nobody waited are caught up.
    [[nodiscard]] ObjectPtr interval_new(std::int64_t period_ms);
    [[nodiscard]] OpPtr interval_tick(const ObjectPtr &interval);
    /// number = the first of `signals` (SIGINT, SIGTERM, ...) delivered to the process.
    [[nodiscard]] OpPtr signal_wait(const std::vector<std::int32_t> &signals);
    [[nodiscard]] OpPtr ctrl_c();

    // ---------------------------------------------------------------------------------------------------------------
    // Streams of byte messages (the `Stream<T>` pipeline; kind "stream")
    // ---------------------------------------------------------------------------------------------------------------
    // A stream is consumed by the combinator or sink that takes it: using it afterwards throws InvalidArgument.
    // Several values packed in one item (chunks, zip, enumerate) use `pack_frames` / `unpack_frames`.
    [[nodiscard]] Bytes pack_frames(const std::vector<Bytes> &frames);
    [[nodiscard]] std::vector<Bytes> unpack_frames(const Bytes &packed); ///< throws InvalidArgument on malformed input

    using StreamMapFn = std::function<Bytes(Bytes)>;
    using StreamPredicate = std::function<bool(const Bytes &)>;
    struct StreamMaybe {
        bool keep = false;
        Bytes value;
    };
    using StreamFilterMapFn = std::function<StreamMaybe(Bytes)>;
    using StreamFoldFn = std::function<Bytes(Bytes accumulator, Bytes item)>;
    using StreamEachFn = std::function<void(const Bytes &)>;
    /// Called for each pull on a blocking-lane thread; may block. Return `keep = false` to end the stream.
    using StreamSourceFn = std::function<StreamMaybe()>;

    [[nodiscard]] ObjectPtr stream_from_channel(const ObjectPtr &receiver);
    [[nodiscard]] ObjectPtr stream_of_items(const std::vector<Bytes> &items);
    [[nodiscard]] ObjectPtr stream_empty();
    /// Items are 8-byte little-endian steady-clock nanoseconds at each tick.
    [[nodiscard]] ObjectPtr stream_interval(std::int64_t period_ms);
    [[nodiscard]] ObjectPtr stream_generate(StreamSourceFn source);
    [[nodiscard]] ObjectPtr stream_map(const ObjectPtr &stream, StreamMapFn fn);
    [[nodiscard]] ObjectPtr stream_filter(const ObjectPtr &stream, StreamPredicate pred);
    [[nodiscard]] ObjectPtr stream_filter_map(const ObjectPtr &stream, StreamFilterMapFn fn);
    [[nodiscard]] ObjectPtr stream_inspect(const ObjectPtr &stream, StreamEachFn fn);
    [[nodiscard]] ObjectPtr stream_take(const ObjectPtr &stream, std::uint64_t count);
    [[nodiscard]] ObjectPtr stream_skip(const ObjectPtr &stream, std::uint64_t count);
    [[nodiscard]] ObjectPtr stream_take_while(const ObjectPtr &stream, StreamPredicate pred);
    /// Items become frames {index as 8-byte little-endian, item}.
    [[nodiscard]] ObjectPtr stream_enumerate(const ObjectPtr &stream);
    /// Items become packed frames of up to `count` items.
    [[nodiscard]] ObjectPtr stream_chunks(const ObjectPtr &stream, std::uint64_t count);
    /// Like `stream_chunks`, but also emits a partial batch once `max_wait_ms` passed since its first item.
    [[nodiscard]] ObjectPtr stream_chunks_timeout(const ObjectPtr &stream, std::uint64_t count, std::int64_t max_wait_ms);
    [[nodiscard]] ObjectPtr stream_chain(const ObjectPtr &first, const ObjectPtr &second);
    [[nodiscard]] ObjectPtr stream_merge(const ObjectPtr &first, const ObjectPtr &second);
    /// Items become frames {a, b}.
    [[nodiscard]] ObjectPtr stream_zip(const ObjectPtr &first, const ObjectPtr &second);
    /// Runs `fn` on the worker pool for up to `concurrency` items at once (0: one per worker); `ordered` keeps the input order.
    [[nodiscard]] ObjectPtr stream_map_concurrent(const ObjectPtr &stream, StreamMapFn fn, std::uint64_t concurrency, bool ordered);
    [[nodiscard]] ObjectPtr stream_throttle(const ObjectPtr &stream, std::int64_t period_ms);
    /// An item that takes longer than `limit_ms` fails the stream's next() with TimedOut.
    [[nodiscard]] ObjectPtr stream_timeout_each(const ObjectPtr &stream, std::int64_t limit_ms);
    /// number = 1 and bytes = the next item, or number = 0 at the end.
    [[nodiscard]] OpPtr stream_next(const ObjectPtr &stream);
    /// Drains the stream: bytes = `pack_frames` of the items, number = how many.
    [[nodiscard]] OpPtr stream_collect(const ObjectPtr &stream);
    [[nodiscard]] OpPtr stream_for_each(const ObjectPtr &stream, StreamEachFn fn);
    [[nodiscard]] OpPtr stream_fold(const ObjectPtr &stream, Bytes initial, StreamFoldFn fn); ///< bytes = the accumulator
    [[nodiscard]] OpPtr stream_count(const ObjectPtr &stream);                                 ///< number
    [[nodiscard]] OpPtr stream_last(const ObjectPtr &stream);                                  ///< number = 1 + bytes, or 0 when empty
    /// Sends every item into a channel; closes nothing.
    [[nodiscard]] OpPtr stream_forward(const ObjectPtr &stream, const ObjectPtr &sender);

    // ---------------------------------------------------------------------------------------------------------------
    // More data parallelism
    // ---------------------------------------------------------------------------------------------------------------
    [[nodiscard]] std::uint32_t par_num_threads();
    /// Runs `a` and `b` in parallel and returns when both are done.
    void par_join(const std::function<void()> &a, const std::function<void()> &b);
    /// Runs `body` with a scope ("par_scope", valid only inside `body`); everything spawned on it finishes before this returns.
    void par_scope(const std::function<void(const ObjectPtr &scope)> &body);
    void par_scope_spawn(const ObjectPtr &scope, std::function<void()> fn);
    /// Calls `body(begin)` for every window start in [0, count - window].
    void par_for_windows(std::uint64_t count, std::uint64_t window, const std::function<void(std::uint64_t)> &body);
    /// Parallel predicates over [0, count): are there / are all / how many; the first index (lowest, or any) for which `pred` holds.
    [[nodiscard]] bool par_any(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred);
    [[nodiscard]] bool par_all(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred);
    [[nodiscard]] std::uint64_t par_count(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred);
    /// -1 when none.
    [[nodiscard]] std::int64_t par_find_first(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred);
    [[nodiscard]] std::int64_t par_find_any(std::uint64_t count, const std::function<bool(std::uint64_t)> &pred);
    /// Maps every index to a 64-bit value and combines them with `reduce` (associative, with `identity`).
    [[nodiscard]] std::int64_t par_map_reduce_i64(std::uint64_t count, const std::function<std::int64_t(std::uint64_t)> &map, std::int64_t identity,
                                                  const std::function<std::int64_t(std::int64_t, std::int64_t)> &reduce);
    [[nodiscard]] double par_map_reduce_f64(std::uint64_t count, const std::function<double(std::uint64_t)> &map, double identity,
                                            const std::function<double(double, double)> &reduce);
    /// Sorts `count` elements of `element_size` bytes in place with `less(a, b)` over pointers to two elements. `stable` keeps equal elements in order.
    void par_sort_by(void *data, std::uint64_t count, std::uint64_t element_size, bool stable, const std::function<bool(const void *, const void *)> &less);
    enum class Reduction { Sum, Product, Min, Max };
    struct NumericValue {
        double f64 = 0;
        std::int64_t i64 = 0;
        std::uint64_t u64 = 0; ///< the field matching the element type holds the result; the others are 0
    };
    /// Reduces a typed array in parallel. False for an empty array with Min/Max.
    [[nodiscard]] bool par_reduce(Numeric type, Reduction reduction, const void *data, std::uint64_t count, NumericValue *out);

    // ---------------------------------------------------------------------------------------------------------------
    // Shared-state primitives for other languages: blocking mutex, SPSC ring buffer, Arc, Promise, execution senders
    // ---------------------------------------------------------------------------------------------------------------
    // These are the engine's own `Mutex<T>`, `SpscRingBuffer<T>`, `Arc<T>`, `Promise<T>` and `Execution` layer, specialised to byte strings
    // so any language can use the same primitive the C++ code does (the Tracy lock visibility, the lock-free ring, one reference count).

    /// A blocking mutex around a byte-string value ("sync_mutex"), with a name for the profiler. Unlike `async_mutex_*` it blocks the calling
    /// thread (never lock it on a reactor thread or hold it across a wait), and a guard must be released on the thread that locked it.
    [[nodiscard]] ObjectPtr sync_mutex_new(Bytes initial, const std::string &debug_name);
    /// Blocks until the lock is free. object = "sync_guard" (use `guard_get` / `guard_set` / `guard_release`).
    [[nodiscard]] ObjectPtr sync_mutex_lock(const ObjectPtr &mutex);
    /// Null when the lock is held.
    [[nodiscard]] ObjectPtr sync_mutex_try_lock(const ObjectPtr &mutex);

    /// Wait-free single-producer single-consumer ring of byte messages ("spsc_ring"). Exactly one thread may push and exactly one (possibly
    /// another) may pop; each message may be at most `max_item_bytes` (rounded up to 32, 128, 512, 2 Ki, 8 Ki or 64 Ki). The capacity is
    /// rounded up to a power of two.
    enum class RingStatus { Ok, Full, Empty, TooLarge };
    [[nodiscard]] ObjectPtr ring_new(std::uint64_t capacity, std::uint64_t max_item_bytes);
    [[nodiscard]] RingStatus ring_try_push(const ObjectPtr &ring, const Bytes &item);
    [[nodiscard]] RingStatus ring_try_pop(const ObjectPtr &ring, Bytes *out);
    /// Pops up to `max_items` (0: all that are there) in order.
    [[nodiscard]] std::vector<Bytes> ring_drain(const ObjectPtr &ring, std::uint64_t max_items);
    [[nodiscard]] std::uint64_t ring_size(const ObjectPtr &ring);
    [[nodiscard]] std::uint64_t ring_capacity(const ObjectPtr &ring);
    [[nodiscard]] std::uint64_t ring_max_item_bytes(const ObjectPtr &ring);

    /// A shared, reference-counted value ("arc"): every clone is another handle to the same data, which is destroyed with the last one. It
    /// holds either bytes (immutable) or a foreign pointer with the function that frees it.
    [[nodiscard]] ObjectPtr arc_new(Bytes data);
    /// `destroy` runs once, when the last reference goes away, on whichever thread dropped it.
    [[nodiscard]] ObjectPtr arc_new_foreign(void *pointer, std::function<void()> destroy);
    [[nodiscard]] ObjectPtr arc_clone(const ObjectPtr &arc);
    /// How many references exist (an instant's snapshot).
    [[nodiscard]] std::uint64_t arc_use_count(const ObjectPtr &arc);
    [[nodiscard]] bool arc_ptr_eq(const ObjectPtr &a, const ObjectPtr &b);
    [[nodiscard]] Bytes arc_bytes(const ObjectPtr &arc);
    /// The foreign pointer (null for a bytes arc).
    [[nodiscard]] void *arc_pointer(const ObjectPtr &arc);

    /// The producing half of an operation ("promise"): something other than the engine finishes `future` by calling `promise_set*`. Only the
    /// first completion wins. A promise released while pending fails its future with Closed.
    struct PromiseEnds {
        ObjectPtr promise;
        OpPtr future;
    };
    [[nodiscard]] PromiseEnds promise_new();
    /// True when this call completed it.
    bool promise_set_value(const ObjectPtr &promise, std::int64_t number, Bytes bytes, std::string text);
    bool promise_set_error(const ObjectPtr &promise, IoErrorCode code, std::string message);
    /// Finishes the future as Cancelled.
    bool promise_cancel(const ObjectPtr &promise);
    [[nodiscard]] bool promise_is_completed(const ObjectPtr &promise);
    /// An already-finished op, like Tokio's `ready`.
    [[nodiscard]] OpPtr op_ready(std::int64_t number, Bytes bytes, std::string text);
    [[nodiscard]] OpPtr op_failed(IoErrorCode code, std::string message);

    /// Lazy descriptions of work that compose and run later (the `Execution` layer): "exec_sender". A sender can be started any number of times.
    using ThenFn = std::function<TaskOutput(ObjectPtr cancel, const TaskOutput &input)>;
    /// A sender that runs `fn` on `lane`.
    [[nodiscard]] ObjectPtr exec_just(TaskFn fn, Lane lane);
    /// A sender that runs `fn` on `lane` with the result of `sender`; a failure upstream skips `fn` and propagates.
    [[nodiscard]] ObjectPtr exec_then(const ObjectPtr &sender, ThenFn fn, Lane lane);
    /// Starts the chain. The op carries the last stage's number / bytes / text; cancelling it sets the token every stage receives.
    [[nodiscard]] OpPtr exec_submit(const ObjectPtr &sender);
    /// Starts the chain and blocks for it; throws `BindingError` if a stage failed. Never call it from a worker thread.
    [[nodiscard]] TaskOutput exec_sync_wait(const ObjectPtr &sender);

    /// A kind name, for diagnostics ("tcp", ...).
    [[nodiscard]] const char *object_kind(const ObjectPtr &object) noexcept;

} // namespace SFT::Async::Binding
