#include <Async/HttpServer.hpp>
#include <Async/Size.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include <Async/FileHandle.hpp>
#include <Async/Http2Engine.hpp>
#include <Async/Http3Engine.hpp>
#include <Async/HttpInternal.hpp>
#include <Async/Tls.hpp>
#include <Async/Sync.hpp>
#include <Async/WebSocketCore.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace SFT::Async::Detail {
    using Literals::operator""_KiB;
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    using tcp = asio::ip::tcp;
    using ErrorCode = boost::system::error_code;
    using TlsContext = asio::ssl::context;

    namespace {
        thread_local bool t_http_server_worker = false;

        [[nodiscard]] unsigned resolve_threads(unsigned requested) noexcept {
            if (requested != 0) {
                return std::clamp(requested, 1u, 64u);
            }
            return std::clamp(std::thread::hardware_concurrency(), 2u, 8u);
        }

        [[nodiscard]] IoError make_io_error(const char *operation, const ErrorCode &error) {
            return IoError{IoErrorCode::Unknown, std::string(operation) + ": " + error.message()};
        }


    } // namespace

    class HttpServerState;

    struct WebSocketRoute {
        std::string path;
        HttpServer::WebSocketHandler handler;
    };

    /// Progress of one streamed response body.
    struct ResponseStreamState {
        HttpBodyStream producer;
        CancellationSource cancel;                  ///< fired when the stream ends or the connection dies
        std::optional<std::uint64_t> remaining;     ///< fixed-length bodies: bytes still owed
        bool chunked = false;
        bool keep_alive = false;
        bool done = false;                          ///< the stream ended or the connection died; stop watching the socket
    };

#if defined(STURDY_ASYNC_HTTP2)
    /// Server ALPN: prefer h2, accept http/1.1, and carry on without ALPN for clients that offer neither.
    inline int select_server_alpn(SSL *, const unsigned char **out, unsigned char *out_length, const unsigned char *in, unsigned in_length, void *) {
        static const unsigned char protocols[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        if (SSL_select_next_proto(const_cast<unsigned char **>(out), out_length, protocols, sizeof(protocols), in, in_length) == OPENSSL_NPN_NEGOTIATED) return SSL_TLSEXT_ERR_OK;
        return SSL_TLSEXT_ERR_NOACK;
    }

    /// Lets the HTTP/2 engine drive the TLS stream an HttpSession already owns.
    template <typename Stream>
    class BeastH2Transport final : public H2Transport, public std::enable_shared_from_this<BeastH2Transport<Stream>> {
      public:
        explicit BeastH2Transport(Stream &stream) : stream_(stream) {}
        [[nodiscard]] asio::any_io_executor executor() override { return stream_.get_executor(); }
        void read_some(std::function<void(bool, const std::byte *, std::size_t)> done) override {
            // A connection that stays completely silent for ten minutes is dropped; any traffic re-arms the timer.
            beast::get_lowest_layer(stream_).expires_after(std::chrono::minutes(10));
            stream_.async_read_some(asio::buffer(buffer_), [self = this->shared_from_this(), done = std::move(done)](const ErrorCode &ec, std::size_t n) {
                done(!ec, reinterpret_cast<const std::byte *>(self->buffer_.data()), n);
            });
        }
        void write(std::vector<std::byte> data, std::function<void(bool)> done) override {
            auto held = std::make_shared<std::vector<std::byte>>(std::move(data));
            beast::get_lowest_layer(stream_).expires_after(std::chrono::minutes(10));
            asio::async_write(stream_, asio::buffer(*held), [self = this->shared_from_this(), held, done = std::move(done)](const ErrorCode &ec, std::size_t) { done(!ec); });
        }
        void close() override {
            ErrorCode ignored;
            beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
            beast::get_lowest_layer(stream_).socket().close(ignored);
        }

      private:
        Stream &stream_;
        std::array<char, 16 * 1024> buffer_{};
    };
#endif

    class HttpSessionControl {
      public:
        virtual ~HttpSessionControl() = default;
        virtual void abort() noexcept = 0;
    };

    template <bool Secure>
    class HttpSession : public HttpSessionControl, public std::enable_shared_from_this<HttpSession<Secure>> {
      public:
        using Stream = std::conditional_t<Secure, beast::ssl_stream<beast::tcp_stream>, beast::tcp_stream>;

        HttpSession(tcp::socket socket, HttpServerState &owner, TlsContext &tls_context,
                    asio::thread_pool &handler_pool, HttpServer::AsyncHandler handler,
                    std::uint64_t max_body_bytes, std::uint32_t max_header_bytes,
                    std::uint64_t max_response_body_bytes)
            : stream_(make_stream(std::move(socket), tls_context)), owner_(owner), handler_pool_(handler_pool),
              handler_(std::move(handler)), max_body_bytes_(max_body_bytes), max_header_bytes_(max_header_bytes),
              max_response_body_bytes_(max_response_body_bytes) {}

        ~HttpSession() override { finish(); }

        void abort() noexcept override {
            auto self = this->shared_from_this();
            asio::post(stream_.get_executor(), [self] { self->finish(); });
        }

        void run();
        void set_client_ip(std::string ip) { ip_ = std::move(ip); }

      private:
        static Stream make_stream(tcp::socket socket, TlsContext &tls_context) {
            if constexpr (Secure) {
                return Stream(std::move(socket), tls_context);
            } else {
                (void)tls_context;
                return Stream(std::move(socket));
            }
        }

        void start_h2();
        void read_request();
        void read_memory_body();
        void read_spooled_body(std::uint64_t length);
        void handle_parsed(http::request<http::string_body> request);
        void reject_parse_error(const ErrorCode &error);
        void cleanup_spool() noexcept;
        void send_streamed(http::status status, unsigned version, const std::vector<HttpHeader> &headers, bool keep_alive, HttpBodyStream producer,
                           std::optional<std::uint64_t> length);
        void stream_next(const std::shared_ptr<ResponseStreamState> &state);
        void stream_chunk(const std::shared_ptr<ResponseStreamState> &state, TaskHandle<std::optional<std::string>> pending);
        void stream_done(const std::shared_ptr<ResponseStreamState> &state);
        void watch_disconnect(const std::shared_ptr<ResponseStreamState> &state);
        void dispatch_request(HttpRequestMessage request, bool keep_alive, unsigned version);
        void write_response(HttpResponseMessage response, bool keep_alive, unsigned version, bool head);
        template <typename Message>
        void send_message(std::shared_ptr<Message> message, bool compute_framing = true);
        void upgrade(http::request<http::string_body> request, const WebSocketRoute &route);
        void close_stream();
        void finish() noexcept;

        Stream stream_;
        beast::flat_buffer buffer_;
        std::unique_ptr<http::request_parser<http::string_body>> parser_;
        HttpServerState &owner_;
        asio::thread_pool &handler_pool_;
        HttpServer::AsyncHandler handler_;
        std::uint64_t max_body_bytes_;
        std::uint32_t max_header_bytes_;
        std::uint64_t max_response_body_bytes_;
        std::string ip_;
        std::string spool_dir_;                                        ///< private directory holding a spooled request body
        std::string spool_path_;                                       ///< the body file inside it
        std::shared_ptr<ResponseStreamState> stream_state_;            ///< the body currently being streamed, if any
#if defined(STURDY_ASYNC_HTTP2)
        std::shared_ptr<H2Engine> h2_;                                 ///< set once the connection negotiated HTTP/2
#endif
        std::unique_ptr<http::request_parser<http::file_body>> spool_parser_;
        bool finished_ = false;
    };

    class HttpServerState {
      public:
        HttpServerState(HttpServerOptions config, HttpServer::AsyncHandler callback)
            : options(std::move(config)), handler(std::move(callback)),
              handler_pool(resolve_threads(options.handler_threads)) {}

        ~HttpServerState() { stop(); }

        std::expected<void, IoError> start() {
            std::lock_guard lock(lifecycle_mutex);
            if (start_attempted) {
                return std::unexpected(IoError{IoErrorCode::InvalidArgument, "HTTP server can only be started once"});
            }
            start_attempted = true;
            if (!handler || options.max_connections == 0 || options.max_request_body_bytes == 0 ||
                options.max_request_header_bytes == 0 || options.max_response_body_bytes == 0 ||
                options.shutdown_timeout_ms == 0 || options.io_threads > 64 || options.handler_threads > 64) {
                return std::unexpected(IoError{IoErrorCode::InvalidArgument, "HTTP server configuration is invalid"});
            }
            const bool has_certificate = !options.tls_certificate_chain_file.empty();
            const bool has_key = !options.tls_private_key_file.empty();
            if (has_certificate != has_key) {
                return std::unexpected(IoError{IoErrorCode::InvalidArgument, "TLS certificate and private key must be provided together"});
            }

            try {
                if (has_certificate) {
                    tls_context.set_options(TlsContext::default_workarounds | TlsContext::no_sslv2 |
                                            TlsContext::no_sslv3 | TlsContext::no_tlsv1 | TlsContext::no_tlsv1_1);
                    if (SSL_CTX_set_min_proto_version(tls_context.native_handle(), TLS1_2_VERSION) != 1) {
                        return std::unexpected(IoError{IoErrorCode::Unknown, "Failed to enforce TLS 1.2 minimum"});
                    }
                    tls_context.use_certificate_chain_file(options.tls_certificate_chain_file);
                    tls_context.use_private_key_file(options.tls_private_key_file, TlsContext::pem);
                    if (SSL_CTX_check_private_key(tls_context.native_handle()) != 1) {
                        return std::unexpected(IoError{IoErrorCode::InvalidArgument, "TLS private key does not match the certificate"});
                    }
#if defined(STURDY_ASYNC_HTTP2)
                    if (options.enable_http2) SSL_CTX_set_alpn_select_cb(tls_context.native_handle(), &select_server_alpn, nullptr);
#endif
                }

                ErrorCode error;
                const auto address = asio::ip::make_address(options.bind_address, error);
                if (error) {
                    return std::unexpected(make_io_error("Invalid HTTP bind address", error));
                }
                const tcp::endpoint endpoint(address, options.port);
                acceptor.open(endpoint.protocol(), error);
                if (error) return std::unexpected(make_io_error("HTTP acceptor open failed", error));
                acceptor.set_option(asio::socket_base::reuse_address(true), error);
                if (error) return std::unexpected(make_io_error("HTTP acceptor option failed", error));
                acceptor.bind(endpoint, error);
                if (error) return std::unexpected(make_io_error("HTTP bind failed", error));
                acceptor.listen(asio::socket_base::max_listen_connections, error);
                if (error) return std::unexpected(make_io_error("HTTP listen failed", error));

                if (options.enable_http3) {
#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
                    if (auto h3 = start_http3(acceptor.local_endpoint().port()); !h3) {
                        ErrorCode ignored;
                        acceptor.close(ignored);
                        return std::unexpected(h3.error());
                    }
#else
                    ErrorCode ignored;
                    acceptor.close(ignored);
                    return std::unexpected(IoError{IoErrorCode::Unsupported, "HTTP/3 is not available in this build (needs ngtcp2 and nghttp3)"});
#endif
                }

                started = true;
                running.store(true, std::memory_order_release);
                accept_next();
                const unsigned count = resolve_threads(options.io_threads);
                io_threads.reserve(count);
                for (unsigned i = 0; i < count; ++i) {
                    io_threads.emplace_back([this] {
                        t_http_server_worker = true;
                        io_context.run();
                        t_http_server_worker = false;
                    });
                }
                return {};
            } catch (const std::exception &exception) {
                running.store(false, std::memory_order_release);
                ErrorCode ignored;
                acceptor.close(ignored);
                io_context.stop();
                for (auto &worker : io_threads) {
                    if (worker.joinable()) worker.join();
                }
                io_threads.clear();
                return std::unexpected(IoError{IoErrorCode::Unknown, exception.what()});
            }
        }

        void stop() noexcept {
            if (t_http_server_worker) {
                running.store(false, std::memory_order_release);
                asio::post(io_context, [this] {
                    close_acceptor();
                    abort_sessions();
                });
                return;
            }
            std::lock_guard lifecycle_lock(lifecycle_mutex);
            if (!started || joined) return;
            running.store(false, std::memory_order_release);
#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
            if (h3_endpoint) h3_endpoint->stop();
#endif
            close_acceptor();
            abort_sessions();
            {
                std::unique_lock session_lock(session_wait_mutex);
                session_wait_cv.wait_for(session_lock, std::chrono::milliseconds(options.shutdown_timeout_ms), [this] {
                    return active_connections.load(std::memory_order_acquire) == 0;
                });
            }
            io_context.stop();
            for (auto &worker : io_threads) {
                if (worker.joinable()) worker.join();
            }
            io_threads.clear();
            handler_pool.join();
            joined = true;
        }

        void close_acceptor() noexcept {
            std::lock_guard lock(acceptor_mutex);
            ErrorCode ignored;
            acceptor.cancel(ignored);
            acceptor.close(ignored);
        }

        void register_session(const std::shared_ptr<HttpSessionControl> &session) {
            std::lock_guard lock(sessions_mutex);
            if (!running.load(std::memory_order_acquire)) {
                session->abort();
                return;
            }
            std::erase_if(sessions, [](const auto &weak_session) { return weak_session.expired(); });
            sessions.emplace_back(session);
        }

        void abort_sessions() noexcept {
            std::vector<std::shared_ptr<HttpSessionControl>> active;
            {
                std::lock_guard lock(sessions_mutex);
                for (auto &weak_session : sessions) {
                    if (auto session = weak_session.lock()) active.push_back(std::move(session));
                }
                sessions.clear();
            }
            for (const auto &session : active) session->abort();
        }

        void session_finished(const std::string &ip = {}) noexcept {
            if (!ip.empty()) release_ip(ip);
            active_connections.fetch_sub(1, std::memory_order_acq_rel);
            session_wait_cv.notify_all();
        }

        /// Counts one more connection from `ip`; false when that address is already at its cap.
        [[nodiscard]] bool reserve_ip(const std::string &ip) {
            if (options.max_connections_per_ip == 0) return true;
            std::lock_guard lock(ip_mutex);
            unsigned &count = connections_by_ip[ip];
            if (count >= options.max_connections_per_ip) return false;
            ++count;
            return true;
        }

        void release_ip(const std::string &ip) noexcept {
            if (options.max_connections_per_ip == 0) return;
            try {
                std::lock_guard lock(ip_mutex);
                if (auto it = connections_by_ip.find(ip); it != connections_by_ip.end() && --it->second == 0) connections_by_ip.erase(it);
            } catch (...) {
            }
        }

        /// Tells HTTP/1.1 and HTTP/2 clients that the same origin also speaks HTTP/3 (RFC 7838).
        void decorate(HttpResponseMessage &response) const {
            if (h3_port == 0) return;
            for (const HttpHeader &h : response.headers) {
                if (iequals_header(h.name, "alt-svc")) return;
            }
            response.headers.push_back({"Alt-Svc", "h3=\":" + std::to_string(h3_port) + "\"; ma=86400"});
        }

        /// Runs the application handler on the handler pool and passes its response to `respond` (any thread).
        /// Shared by HTTP/2 and HTTP/3, which both deliver complete requests from a protocol engine.
        void dispatch_async(HttpRequestMessage request, std::function<void(HttpResponseMessage)> respond, bool advertise_h3) {
            ++requests;
            asio::post(handler_pool, [this, request = std::move(request), respond = std::move(respond), advertise_h3]() mutable {
                auto internal_error = [] {
                    HttpResponseMessage response;
                    response.status = 500;
                    response.body = "Internal Server Error";
                    return response;
                };
                t_http_server_worker = true;
                TaskHandle<HttpResponseMessage> pending;
                try {
                    pending = handler(std::move(request));
                } catch (...) {
                    pending = ready(internal_error());
                }
                t_http_server_worker = false;
                if (!pending.valid()) pending = ready(internal_error());
                pending.on_done([this, pending, respond = std::move(respond), internal_error, advertise_h3]() mutable {
                    HttpResponseMessage response;
                    try {
                        response = pending.wait();
                    } catch (...) {
                        response = internal_error();
                    }
                    normalize_file_response(response);
                    if (advertise_h3) decorate(response);
                    respond(std::move(response));
                });
            });
        }

        void count_response(unsigned status) noexcept {
            if (status >= 500) ++responses_5xx;
            else if (status >= 400) ++responses_4xx;
            else if (status >= 300) ++responses_3xx;
            else ++responses_2xx;
        }

        template <bool Secure>
        void create_session(tcp::socket socket, std::string ip) {
            std::shared_ptr<HttpSession<Secure>> session;
            try {
                session = std::make_shared<HttpSession<Secure>>(std::move(socket), *this, tls_context, handler_pool,
                    handler, options.max_request_body_bytes, options.max_request_header_bytes,
                    options.max_response_body_bytes);
                session->set_client_ip(ip);
            } catch (...) {
                session_finished(ip);
                return;
            }
            try {
                register_session(session);
                session->run();
            } catch (...) {
                session->abort();
            }
        }

        void accept_next() {
            if (!running.load(std::memory_order_acquire)) return;
            std::lock_guard accept_lock(acceptor_mutex);
            if (!running.load(std::memory_order_acquire)) return;
            acceptor.async_accept(asio::make_strand(io_context), [this](const ErrorCode &error, tcp::socket socket) {
                if (!error && running.load(std::memory_order_acquire)) {
                    const unsigned previous = active_connections.fetch_add(1, std::memory_order_acq_rel);
                    ErrorCode endpoint_error;
                    const auto remote = socket.remote_endpoint(endpoint_error);
                    // An IPv4-mapped IPv6 address and the plain IPv4 one are the same client.
                    std::string ip;
                    if (!endpoint_error) {
                        auto address = remote.address();
                        if (address.is_v6() && address.to_v6().is_v4_mapped()) address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
                        ip = address.to_string();
                    }
                    if (previous >= options.max_connections) {
                        ++rejected_limit;
                        session_finished();
                        ErrorCode ignored;
                        socket.close(ignored);
                    } else if (!ip.empty() && !reserve_ip(ip)) {
                        ++rejected_per_ip;
                        session_finished();
                        ErrorCode ignored;
                        socket.close(ignored);
                    } else {
                        ++accepted;
                        if (tls_enabled()) {
                            create_session<true>(std::move(socket), ip);
                        } else {
                            create_session<false>(std::move(socket), ip);
                        }
                    }
                }
                if (running.load(std::memory_order_acquire)) accept_next();
            });
        }

        [[nodiscard]] bool tls_enabled() const noexcept {
            return !options.tls_certificate_chain_file.empty();
        }

        /// The route whose path equals `target` (query ignored), or null.
        [[nodiscard]] const WebSocketRoute *find_websocket_route(const std::string &target) const {
            const std::string path = target.substr(0, target.find('?'));
            for (const WebSocketRoute &route : websocket_routes) {
                if (route.path == path) return &route;
            }
            return nullptr;
        }

#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
        /// Starts the QUIC listener for HTTP/3 next to the TCP one. Called with the TCP port already bound.
        [[nodiscard]] std::expected<void, IoError> start_http3(std::uint16_t tcp_port) {
            if (!tls_enabled()) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "HTTP/3 needs TLS (set the certificate and key files)"});
            auto certificate = read_text_file(options.tls_certificate_chain_file);
            auto key = read_text_file(options.tls_private_key_file);
            if (!certificate) return std::unexpected(certificate.error());
            if (!key) return std::unexpected(key.error());
            TlsServerOptions tls;
            tls.certificate_chain_pem = std::move(*certificate);
            tls.private_key_pem = std::move(*key);

            H3ServerConfig config;
            config.max_request_body_bytes = options.max_request_body_bytes;
            config.max_header_bytes = options.max_request_header_bytes;
            config.max_response_body_bytes = options.max_response_body_bytes;
            config.on_response = [this](unsigned status) { count_response(status); };
            config.dispatch = [this](HttpRequestMessage request, std::function<void(HttpResponseMessage)> respond) {
                dispatch_async(std::move(request), std::move(respond), false);
            };
            ErrorCode error;
            const auto address = asio::ip::make_address(options.bind_address, error);
            if (error) return std::unexpected(make_io_error("Invalid HTTP bind address", error));
            const std::uint16_t port = options.http3_port != 0 ? options.http3_port : tcp_port;
            auto endpoint = start_h3_server(udp::endpoint(address, port), std::move(tls), std::move(config), options.http3_max_concurrent_streams,
                                            options.http3_max_connections, options.http3_max_connections_per_address);
            if (!endpoint) return std::unexpected(endpoint.error());
            h3_endpoint = *endpoint;
            h3_port = h3_endpoint->local_port();
            return {};
        }
        std::shared_ptr<QuicEndpoint> h3_endpoint;
#endif
        std::uint16_t h3_port = 0; ///< set before the first connection is accepted; 0 = no HTTP/3

        HttpServerOptions options;
        HttpServer::AsyncHandler handler;
        std::vector<WebSocketRoute> websocket_routes;
        std::shared_ptr<std::atomic<std::size_t>> websocket_live = std::make_shared<std::atomic<std::size_t>>(0);
        std::atomic<unsigned> active_connections{0};
        std::mutex ip_mutex;
        std::map<std::string, unsigned> connections_by_ip;
        std::atomic<std::uint64_t> accepted{0}, rejected_limit{0}, rejected_per_ip{0}, requests{0};
        std::atomic<std::uint64_t> responses_2xx{0}, responses_3xx{0}, responses_4xx{0}, responses_5xx{0}, websocket_upgrades{0};
        std::atomic<bool> running{false};
        std::mutex lifecycle_mutex;
        std::mutex acceptor_mutex;
        std::mutex sessions_mutex;
        std::vector<std::weak_ptr<HttpSessionControl>> sessions;
        std::mutex session_wait_mutex;
        std::condition_variable session_wait_cv;
        bool start_attempted = false;
        bool started = false;
        bool joined = false;
        asio::io_context io_context;
        TlsContext tls_context{TlsContext::tls_server};
        tcp::acceptor acceptor{io_context};
        asio::thread_pool handler_pool;
        std::vector<std::thread> io_threads;
    };

    template <bool Secure>
    void HttpSession<Secure>::run() {
        if constexpr (Secure) {
            beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(10));
            stream_.async_handshake(asio::ssl::stream_base::server, [self = this->shared_from_this()](const ErrorCode &error) {
                if (error) return self->finish();
#if defined(STURDY_ASYNC_HTTP2)
                const unsigned char *protocol = nullptr;
                unsigned protocol_length = 0;
                SSL_get0_alpn_selected(self->stream_.native_handle(), &protocol, &protocol_length);
                if (self->owner_.options.enable_http2 && protocol_length == 2 && protocol != nullptr && protocol[0] == 'h' && protocol[1] == '2') {
                    return self->start_h2();
                }
#endif
                self->read_request();
            });
        } else {
            read_request();
        }
    }

    template <bool Secure>
    void HttpSession<Secure>::start_h2() {
#if defined(STURDY_ASYNC_HTTP2)
        if constexpr (Secure) {
            auto transport = std::make_shared<BeastH2Transport<Stream>>(stream_);
            H2ServerConfig config;
            config.max_request_body_bytes = max_body_bytes_;
            config.max_header_bytes = max_header_bytes_;
            config.max_response_body_bytes = max_response_body_bytes_;
            config.max_concurrent_streams = owner_.options.http2_max_concurrent_streams;
            config.remote_address = ip_;
            config.on_response = [this_ = this->shared_from_this()](unsigned status) { this_->owner_.count_response(status); };
            config.dispatch = [this_ = this->shared_from_this()](HttpRequestMessage request, std::function<void(HttpResponseMessage)> respond) {
                this_->owner_.dispatch_async(std::move(request), std::move(respond), true);
            };
            // The engine owns the connection from here on; when it ends, the session ends too.
            h2_ = H2Engine::make_server(transport, std::move(config), [self = this->shared_from_this()] {
                auto engine = std::move(self->h2_);
                self->finish();
            });
            h2_->start();
        } else {
            finish();
        }
#else
        finish();
#endif
    }

    template <bool Secure>
    void HttpSession<Secure>::reject_parse_error(const ErrorCode &error) {
        // Clients that exceeded a limit or sent garbage get a proper status before the connection is dropped;
        // timeouts and disconnects just end the session.
        cleanup_spool();
        unsigned status = 0;
        if (error == http::error::body_limit) status = 413;
        else if (error == http::error::header_limit) status = 431;
        else if (error.category() == http::make_error_code(http::error::bad_version).category() && error != http::error::end_of_stream) status = 400;
        if (status == 0) return finish();
        HttpResponseMessage rejection;
        rejection.status = status;
        rejection.body = status == 413 ? "Payload Too Large" : status == 431 ? "Request Header Fields Too Large" : "Bad Request";
        rejection.keep_alive = false;
        write_response(std::move(rejection), false, 11, false);
    }

    template <bool Secure>
    void HttpSession<Secure>::read_request() {
        cleanup_spool();
        parser_ = std::make_unique<http::request_parser<http::string_body>>();
        parser_->body_limit(max_body_bytes_);
        parser_->header_limit(max_header_bytes_);
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
        // Headers first: they say how big the body is, which decides whether it lives in memory or on disk.
        http::async_read_header(stream_, buffer_, *parser_, [self = this->shared_from_this()](const ErrorCode &error, std::size_t) {
            if (error) return self->reject_parse_error(error);
            const auto length = self->parser_->content_length();
            const std::uint64_t threshold = self->owner_.options.spool_requests_above;
            if (threshold > 0 && length && *length > threshold) return self->read_spooled_body(*length);
            self->read_memory_body();
        });
    }

    template <bool Secure>
    void HttpSession<Secure>::read_memory_body() {
        http::async_read(stream_, buffer_, *parser_, [self = this->shared_from_this()](const ErrorCode &error, std::size_t) {
            if (error) return self->reject_parse_error(error);
            self->handle_parsed(self->parser_->release());
        });
    }

    template <bool Secure>
    void HttpSession<Secure>::read_spooled_body(std::uint64_t length) {
        if (length > owner_.options.max_spooled_body_bytes) return reject_parse_error(http::error::body_limit);
        // The upload goes to a private (0700) directory: Beast creates files world-readable, the directory is what protects them.
        namespace fs = std::filesystem;
        std::error_code fs_error;
        std::random_device entropy;
        char suffix[17];
        std::snprintf(suffix, sizeof(suffix), "%08x%08x", entropy(), entropy());
        const fs::path directory = fs::temp_directory_path(fs_error) / (std::string("sturdy-upload-") + suffix);
        fs::create_directory(directory, fs_error);
        if (!fs_error) fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, fs_error);
        if (fs_error) return reject_parse_error(http::error::bad_alloc);
        spool_dir_ = directory.string();
        spool_path_ = (directory / "body").string();

        spool_parser_ = std::make_unique<http::request_parser<http::file_body>>(std::move(*parser_));
        spool_parser_->body_limit(owner_.options.max_spooled_body_bytes);
        ErrorCode open_error;
        spool_parser_->get().body().open(spool_path_.c_str(), beast::file_mode::write, open_error);
        if (open_error) return reject_parse_error(http::error::bad_alloc);
        http::async_read(stream_, buffer_, *spool_parser_, [self = this->shared_from_this()](const ErrorCode &error, std::size_t) {
            if (error) return self->reject_parse_error(error);
            auto request = self->spool_parser_->release();
            request.body().close();
            HttpRequestMessage message;
            message.method = std::string(request.method_string());
            message.target = std::string(request.target());
            message.version = request.version();
            message.body_file = self->spool_path_;
            message.remote_address = self->ip_;
            for (const auto &field : request.base()) message.headers.push_back({std::string(field.name_string()), std::string(field.value())});
            ++self->owner_.requests;
            self->dispatch_request(std::move(message), request.keep_alive(), request.version());
        });
    }

    template <bool Secure>
    void HttpSession<Secure>::cleanup_spool() noexcept {
        spool_parser_.reset();
        if (!spool_dir_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(spool_dir_, ignored);
            spool_dir_.clear();
            spool_path_.clear();
        }
    }

    template <bool Secure>
    void HttpSession<Secure>::handle_parsed(http::request<http::string_body> request) {
        ++owner_.requests;
        if (beast::websocket::is_upgrade(request)) {
            if (const auto *route = owner_.find_websocket_route(std::string(request.target()))) {
                return upgrade(std::move(request), *route);
            }
        }
        HttpRequestMessage message;
        message.method = std::string(request.method_string());
        message.target = std::string(request.target());
        message.version = request.version();
        message.body = std::move(request.body());
        message.remote_address = ip_;
        for (const auto &field : request.base()) {
            message.headers.push_back({std::string(field.name_string()), std::string(field.value())});
        }
        dispatch_request(std::move(message), request.keep_alive(), request.version());
    }

    template <bool Secure>
    void HttpSession<Secure>::dispatch_request(HttpRequestMessage request, bool keep_alive, unsigned version) {
        auto self = this->shared_from_this();
        const bool head = request.method == "HEAD";
        auto internal_error = [] {
            HttpResponseMessage response;
            response.status = 500;
            response.body = "Internal Server Error";
            response.keep_alive = false;
            return response;
        };
        try {
            asio::post(handler_pool_, [self, request = std::move(request), keep_alive, version, head, internal_error]() mutable {
                t_http_server_worker = true;
                TaskHandle<HttpResponseMessage> pending;
                try {
                    pending = self->handler_(std::move(request));
                } catch (...) {
                    pending = ready(internal_error());
                }
                t_http_server_worker = false;
                if (!pending.valid()) pending = ready(internal_error());
                // The pool thread is released here; the response is written when the handler's task completes.
                pending.on_done([self, pending, keep_alive, version, head, internal_error]() mutable {
                    HttpResponseMessage response;
                    try {
                        response = pending.wait();
                    } catch (...) {
                        response = internal_error();
                    }
                    normalize_file_response(response); // stat the file here, off the reactor thread
                    self->owner_.decorate(response);
                    asio::post(self->stream_.get_executor(), [self, response = std::move(response), keep_alive, version, head]() mutable {
                        self->write_response(std::move(response), keep_alive, version, head);
                    });
                });
            });
        } catch (...) {
            write_response(internal_error(), false, version, head);
        }
    }

    template <bool Secure>
    template <typename Message>
    void HttpSession<Secure>::send_message(std::shared_ptr<Message> message, bool compute_framing) {
        // Responses that carry an explicit Content-Length without a body (HEAD, 304) must not be "corrected" to 0.
        if (compute_framing) message->prepare_payload();
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
        http::async_write(stream_, *message, [self = this->shared_from_this(), message](const ErrorCode &error, std::size_t) {
            if (error) return self->finish();
            if (message->keep_alive()) {
                self->read_request();
            } else {
                self->close_stream();
            }
        });
    }

    template <bool Secure>
    void HttpSession<Secure>::write_response(HttpResponseMessage response, bool keep_alive, unsigned version, bool head) {
        owner_.count_response(response.status);
        const auto status = static_cast<http::status>(response.status >= 100 && response.status <= 599 ? response.status : 500);

        // Anything that would corrupt the wire format is refused outright: a header value with a line break would let a
        // handler (or data it echoes) split the response.
        auto bad_text = [](const std::string &text) { return text.find_first_of(std::string("\r\n\0", 3)) != std::string::npos; };
        bool invalid_headers = false;
        std::uint64_t header_bytes = 0;
        for (const HttpHeader &header : response.headers) {
            header_bytes += header.name.size() + header.value.size();
            if (header.name.empty() || bad_text(header.name) || bad_text(header.value) || header.name.find(':') != std::string::npos ||
                header_bytes > 32_KiB) {
                invalid_headers = true;
                break;
            }
        }
        const bool use_file = !response.body_file.empty();
        const bool response_too_large = !use_file && response.body.size() > max_response_body_bytes_;
        const bool serving_file_body = use_file && !invalid_headers;
        const bool must_replace = invalid_headers || response_too_large;
        const bool final_keep_alive = keep_alive && response.keep_alive && !must_replace && owner_.running.load(std::memory_order_acquire);

        auto fill_headers = [&](auto &message) {
            message.keep_alive(final_keep_alive);
            message.set(http::field::server, "SturdyEngine Async HTTP");
            if (must_replace) return;
            for (const HttpHeader &header : response.headers) {
                // Framing headers are computed, never taken from handlers.
                if (header.name.size() == 14 && std::equal(header.name.begin(), header.name.end(), "Content-Length", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) continue;
                if (header.name.size() == 17 && std::equal(header.name.begin(), header.name.end(), "Transfer-Encoding", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) continue;
                message.insert(header.name, header.value);
            }
        };

        try {
            if (must_replace) {
                auto message = std::make_shared<http::response<http::string_body>>(http::status::internal_server_error, version);
                message->body() = invalid_headers ? "Response headers are invalid or exceed the safety limit" : "Response body exceeds configured safety limit";
                fill_headers(*message);
                if (head) message->body().clear();
                return send_message(std::move(message));
            }

            // Streamed bodies: producer-driven (SSE, generated data) or a file read on the IO lane -- never on this thread.
            if (response.body_stream && !head && status != http::status::not_modified && status != http::status::no_content) {
                return send_streamed(status, version, response.headers, final_keep_alive, std::move(response.body_stream), std::nullopt);
            }
            if (serving_file_body) {
                const std::uint64_t file_length = response.body_length.value_or(0);
                if (head || status == http::status::not_modified || status == http::status::no_content) {
                    auto message = std::make_shared<http::response<http::empty_body>>(status, version);
                    fill_headers(*message);
                    if (status != http::status::not_modified && status != http::status::no_content) message->content_length(file_length);
                    return send_message(std::move(message), false);
                }
                return send_streamed(status, version, response.headers, final_keep_alive,
                                     make_file_producer(response.body_file, response.body_offset, file_length), file_length);
            }

            if (head) {
                // HEAD: the headers a GET would produce, including its Content-Length, and no body.
                auto message = std::make_shared<http::response<http::empty_body>>(status, version);
                fill_headers(*message);
                std::uint64_t length = response.body.size();
                for (const HttpHeader &header : response.headers) {
                    if (header.name.size() == 14 && header.value.find_first_not_of("0123456789") == std::string::npos && !header.value.empty() &&
                        std::equal(header.name.begin(), header.name.end(), "Content-Length", [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
                        length = std::strtoull(header.value.c_str(), nullptr, 10);
                    }
                }
                if (status != http::status::not_modified && status != http::status::no_content) message->content_length(length);
                return send_message(std::move(message), false);
            }

            auto message = std::make_shared<http::response<http::string_body>>(status, version);
            message->body() = std::move(response.body);
            fill_headers(*message);
            send_message(std::move(message));
        } catch (...) {
            auto message = std::make_shared<http::response<http::string_body>>(http::status::internal_server_error, version);
            message->body() = "Internal Server Error";
            message->keep_alive(false);
            send_message(std::move(message));
        }
    }

    template <bool Secure>
    void HttpSession<Secure>::send_streamed(http::status status, unsigned version, const std::vector<HttpHeader> &headers, bool keep_alive,
                                            HttpBodyStream producer, std::optional<std::uint64_t> length) {
        auto state = std::make_shared<ResponseStreamState>();
        state->producer = std::move(producer);
        state->remaining = length;
        // HTTP/1.0 clients cannot be sent chunked data: delimit the body by closing the connection instead.
        state->chunked = !length && version >= 11;
        state->keep_alive = keep_alive && (length.has_value() || state->chunked);
        stream_state_ = state;

        auto header = std::make_shared<http::response<http::empty_body>>(status, version);
        header->keep_alive(state->keep_alive);
        header->set(http::field::server, "SturdyEngine Async HTTP");
        for (const HttpHeader &h : headers) {
            if (iequals_header(h.name, "content-length") || iequals_header(h.name, "transfer-encoding")) continue;
            header->insert(h.name, h.value);
        }
        if (length) header->content_length(*length);
        else if (state->chunked) header->chunked(true);
        auto serializer = std::make_shared<http::response_serializer<http::empty_body>>(*header);
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
        http::async_write_header(stream_, *serializer, [self = this->shared_from_this(), header, serializer, state](const ErrorCode &error, std::size_t) {
            if (error) {
                (void)state->cancel.cancel();
                return self->finish();
            }
            self->watch_disconnect(state);
            self->stream_next(state);
        });
    }

    /// While a body is being produced the connection is otherwise silent, so a client that leaves would go unnoticed until
    /// the next write. A readiness wait + MSG_PEEK spots the FIN without consuming bytes a client may have pipelined.
    template <bool Secure>
    void HttpSession<Secure>::watch_disconnect(const std::shared_ptr<ResponseStreamState> &state) {
        auto &socket = beast::get_lowest_layer(stream_).socket();
        socket.async_wait(tcp::socket::wait_read, [self = this->shared_from_this(), state](const ErrorCode &wait_error) {
            if (wait_error || state->done || self->finished_) return;
            ErrorCode peek_error;
            char probe = 0;
            const std::size_t n = beast::get_lowest_layer(self->stream_).socket().receive(asio::buffer(&probe, 1), tcp::socket::message_peek, peek_error);
            if (n == 0 || (peek_error && peek_error != asio::error::would_block)) {
                (void)state->cancel.cancel();
                self->finish();
            } // else: pipelined request bytes; leave them and stop watching
        });
    }

    template <bool Secure>
    void HttpSession<Secure>::stream_next(const std::shared_ptr<ResponseStreamState> &state) {
        // Waiting for the producer (an event source, a disk read) is not an idle connection: no write deadline meanwhile.
        beast::get_lowest_layer(stream_).expires_never();
        TaskHandle<std::optional<std::string>> pending;
        try {
            pending = state->producer(state->cancel.token());
        } catch (...) {
            (void)state->cancel.cancel();
            return finish();
        }
        if (!pending.valid()) return finish();
        pending.on_done([self = this->shared_from_this(), pending, state]() mutable {
            asio::post(self->stream_.get_executor(), [self, pending, state]() mutable { self->stream_chunk(state, pending); });
        });
    }

    template <bool Secure>
    void HttpSession<Secure>::stream_chunk(const std::shared_ptr<ResponseStreamState> &state, TaskHandle<std::optional<std::string>> pending) {
        if (finished_) return;
        std::optional<std::string> item;
        try {
            item = pending.wait();
        } catch (...) {
            (void)state->cancel.cancel();
            return finish(); // the headers are out already; the only honest signal left is to drop the connection
        }
        if (!item) {
            if (state->remaining && *state->remaining != 0) { // promised more than it delivered
                (void)state->cancel.cancel();
                return finish();
            }
            if (state->chunked) {
                beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
                return asio::async_write(stream_, http::make_chunk_last(), [self = this->shared_from_this(), state](const ErrorCode &error, std::size_t) {
                    if (error) return self->finish();
                    self->stream_done(state);
                });
            }
            return stream_done(state);
        }
        if (item->empty()) return stream_next(state); // an empty chunk would terminate a chunked body; skip it
        if (state->remaining) {
            if (item->size() > *state->remaining) { // more than the declared Content-Length: never send it
                (void)state->cancel.cancel();
                return finish();
            }
            *state->remaining -= item->size();
        }
        auto data = std::make_shared<std::string>(std::move(*item));
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(30));
        auto written = [self = this->shared_from_this(), state, data](const ErrorCode &error, std::size_t) {
            if (error) {
                (void)state->cancel.cancel();
                return self->finish();
            }
            if (state->remaining && *state->remaining == 0) return self->stream_done(state);
            self->stream_next(state);
        };
        if (state->chunked) asio::async_write(stream_, http::make_chunk(asio::buffer(*data)), std::move(written));
        else asio::async_write(stream_, asio::buffer(*data), std::move(written));
    }

    template <bool Secure>
    void HttpSession<Secure>::stream_done(const std::shared_ptr<ResponseStreamState> &state) {
        state->done = true;
        ErrorCode ignored;
        beast::get_lowest_layer(stream_).socket().cancel(ignored); // ends the disconnect watch
        (void)state->cancel.cancel();
        state->producer = nullptr;
        stream_state_.reset();
        if (state->keep_alive && owner_.running.load(std::memory_order_acquire)) read_request();
        else close_stream();
    }

    template <bool Secure>
    void HttpSession<Secure>::upgrade(http::request<http::string_body> request, const WebSocketRoute &route) {
        const std::string origin(request[http::field::origin]);
        const auto &allowed = owner_.options.websocket_allowed_origins;
        auto refuse = [&](http::status status, const char *why) {
            HttpResponseMessage response;
            response.status = static_cast<unsigned>(status);
            response.body = why;
            response.keep_alive = false;
            write_response(std::move(response), false, request.version(), false);
        };
        if (!allowed.empty() && !origin.empty() && std::find(allowed.begin(), allowed.end(), origin) == allowed.end()) {
            return refuse(http::status::forbidden, "origin not allowed");
        }
        if (owner_.websocket_live->load(std::memory_order_acquire) >= owner_.options.max_websockets) {
            return refuse(http::status::service_unavailable, "too many WebSocket connections");
        }

        // Subprotocol: the first server preference the client also offered.
        std::string chosen;
        const std::string offered(request[http::field::sec_websocket_protocol]);
        for (const auto &preference : owner_.options.websocket_subprotocols) {
            std::size_t start = 0;
            while (start <= offered.size() && chosen.empty()) {
                auto comma = offered.find(',', start);
                if (comma == std::string::npos) comma = offered.size();
                std::string item = offered.substr(start, comma - start);
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t") + 1);
                if (item == preference) chosen = preference;
                start = comma + 1;
            }
            if (!chosen.empty()) break;
        }

        HttpRequestMessage info;
        info.method = std::string(request.method_string());
        info.target = std::string(request.target());
        info.version = request.version();
        for (const auto &field : request.base()) info.headers.push_back({std::string(field.name_string()), std::string(field.value())});

        // Hand the transport over to the WebSocket and stop being an HTTP session without closing the socket.
        Result<SocketAddress> peer = std::unexpected(IoError{IoErrorCode::Unknown, "no peer"});
        {
            ErrorCode ec;
            const auto endpoint = beast::get_lowest_layer(stream_).socket().remote_endpoint(ec);
            if (!ec) peer = SocketAddress{endpoint.address().to_string(), endpoint.port()};
        }
        Result<TlsInfo> tls = std::unexpected(IoError{IoErrorCode::InvalidArgument, "connection is not using TLS"});
        if constexpr (Secure) tls = describe_tls(stream_.native_handle());

        finished_ = true;
        ++owner_.websocket_upgrades;
        owner_.session_finished(ip_);
        beast::get_lowest_layer(stream_).expires_never();
        auto session = std::make_shared<WsSession<Stream>>(owner_.options.websocket, std::move(stream_));
        session->live = owner_.websocket_live;
        owner_.websocket_live->fetch_add(1, std::memory_order_acq_rel);
        session->peer_provider = [peer] { return peer; };
        if constexpr (Secure) session->tls_provider = [tls] { return tls; };
        session->request.target = info.target;
        session->request.headers = info.headers;
        session->request.origin = origin;
        session->request.subprotocol = chosen;
        session->ws.set_option(beast::websocket::stream_base::decorator([chosen](beast::websocket::response_type &response) {
            response.set(http::field::server, "SturdyEngine Async HTTP");
            if (!chosen.empty()) response.set(http::field::sec_websocket_protocol, chosen);
        }));
        auto handler = route.handler;
        asio::post(session->executor(), [session, request = std::move(request), handler, info = std::move(info), &pool = handler_pool_]() mutable {
            session->ws.async_accept(request, [session, handler, info = std::move(info), &pool](const ErrorCode &ec) mutable {
                if (ec) return session->abort();
                session->start();
                asio::post(pool, [session, handler, info = std::move(info)]() mutable {
                    handler(WsFactory::make(session), std::move(info));
                });
            });
        });
    }

    template <bool Secure>
    void HttpSession<Secure>::close_stream() {
        if constexpr (Secure) {
            beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(2));
            stream_.async_shutdown([self = this->shared_from_this()](const ErrorCode &) { self->finish(); });
        } else {
            finish();
        }
    }

    template <bool Secure>
    void HttpSession<Secure>::finish() noexcept {
        if (finished_) return;
        finished_ = true;
        ErrorCode ignored;
        beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(stream_).socket().close(ignored);
        if (stream_state_) {
            stream_state_->done = true;
            (void)stream_state_->cancel.cancel(); // wake a producer that is waiting for events
            stream_state_->producer = nullptr;     // and release what it captured (subscriptions, files)
            stream_state_.reset();
        }
        cleanup_spool();
        owner_.session_finished(ip_);
    }
} // namespace SFT::Async::Detail

namespace SFT::Async {
    HttpServer::HttpServer(HttpServerOptions options, Handler handler)
        : HttpServer(std::move(options), AsyncHandler{[handler = std::move(handler)](HttpRequestMessage request) {
              // Runs on the handler pool; its result (or exception) is delivered through the returned handle.
              Promise<HttpResponseMessage> promise;
              try {
                  promise.set_value(handler(std::move(request)));
              } catch (...) {
                  promise.set_exception(std::current_exception());
              }
              return promise.handle();
          }}) {}

    HttpServer::HttpServer(HttpServerOptions options, AsyncHandler handler)
        : state_(std::make_unique<Detail::HttpServerState>(std::move(options), std::move(handler))) {}

    void HttpServer::on_websocket(std::string path, WebSocketHandler handler) {
        if (state_ && handler) state_->websocket_routes.push_back({std::move(path), std::move(handler)});
    }

    HttpServer::~HttpServer() { stop(); }
    HttpServer::HttpServer(HttpServer &&) noexcept = default;
    HttpServer &HttpServer::operator=(HttpServer &&) noexcept = default;

    std::expected<void, IoError> HttpServer::start() {
        if (!state_) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "HTTP server has no state"});
        return state_->start();
    }

    void HttpServer::stop() noexcept {
        if (state_) state_->stop();
    }

    bool HttpServer::is_running() const noexcept {
        return state_ && state_->running.load(std::memory_order_acquire);
    }

    std::expected<std::uint16_t, IoError> HttpServer::local_port() const {
        if (!state_) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "HTTP server has no state"});
        std::lock_guard lock(state_->acceptor_mutex);
        boost::system::error_code error;
        const auto endpoint = state_->acceptor.local_endpoint(error);
        if (error) return std::unexpected(IoError{IoErrorCode::Unknown, "HTTP server endpoint unavailable: " + error.message()});
        return endpoint.port();
    }

    bool HttpServer::uses_tls() const noexcept {
        return state_ && state_->tls_enabled();
    }

    bool HttpServer::offers_http2() const noexcept {
#if defined(STURDY_ASYNC_HTTP2)
        return state_ && state_->tls_enabled() && state_->options.enable_http2;
#else
        return false;
#endif
    }

    std::expected<std::uint16_t, IoError> HttpServer::http3_local_port() const {
        if (!state_ || state_->h3_port == 0) return std::unexpected(IoError{IoErrorCode::Unsupported, "HTTP/3 is not enabled"});
        return state_->h3_port;
    }

    HttpServerMetrics HttpServer::metrics() const noexcept {
        HttpServerMetrics m;
        if (!state_) return m;
        m.connections_accepted = state_->accepted.load();
        m.connections_rejected_limit = state_->rejected_limit.load();
        m.connections_rejected_per_ip = state_->rejected_per_ip.load();
        m.requests = state_->requests.load();
        m.responses_2xx = state_->responses_2xx.load();
        m.responses_3xx = state_->responses_3xx.load();
        m.responses_4xx = state_->responses_4xx.load();
        m.responses_5xx = state_->responses_5xx.load();
        m.websocket_upgrades = state_->websocket_upgrades.load();
        m.active_connections = state_->active_connections.load();
        m.active_websockets = state_->websocket_live->load();
        return m;
    }
} // namespace SFT::Async

#else

namespace SFT::Async::Detail {
    class HttpServerState {};
}

namespace SFT::Async {
    HttpServer::HttpServer(HttpServerOptions, Handler) : state_(std::make_unique<Detail::HttpServerState>()) {}
    HttpServer::HttpServer(HttpServerOptions, AsyncHandler) : state_(std::make_unique<Detail::HttpServerState>()) {}
    void HttpServer::on_websocket(std::string, WebSocketHandler) {}
    HttpServer::~HttpServer() = default;
    HttpServer::HttpServer(HttpServer &&) noexcept = default;
    HttpServer &HttpServer::operator=(HttpServer &&) noexcept = default;
    std::expected<void, IoError> HttpServer::start() {
        return std::unexpected(IoError{IoErrorCode::Unsupported, "HTTP server hosting is not available on Web"});
    }
    void HttpServer::stop() noexcept {}
    bool HttpServer::is_running() const noexcept { return false; }
    std::expected<std::uint16_t, IoError> HttpServer::local_port() const {
        return std::unexpected(IoError{IoErrorCode::Unsupported, "HTTP server hosting is not available on Web"});
    }
    bool HttpServer::uses_tls() const noexcept { return false; }
    HttpServerMetrics HttpServer::metrics() const noexcept { return {}; }
    bool HttpServer::offers_http2() const noexcept { return false; }
    std::expected<std::uint16_t, IoError> HttpServer::http3_local_port() const {
        return std::unexpected(IoError{IoErrorCode::Unsupported, "HTTP server hosting is not available on Web"});
    }
}

#endif
