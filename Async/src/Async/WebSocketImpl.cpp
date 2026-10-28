#include <Async/WebSocket.hpp>
#include <Async/Promise.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <Async/UrlInternal.hpp>
#include <Async/WebSocketCore.hpp>

#include <boost/beast/http.hpp>

#include <algorithm>
#include <cctype>

namespace SFT::Async {

    namespace Detail {
        namespace http = beast::http;

        IoError handshake_error(const ErrorCode &ec) {
            if (ec.category() == websocket::make_error_code(websocket::error::bad_opcode).category() ||
                ec.category() == http::make_error_code(http::error::bad_version).category()) {
                return IoError{IoErrorCode::ProtocolError, "WebSocket upgrade refused: " + ec.message()};
            }
            if (ec == beast::error::timeout) return IoError{IoErrorCode::TimedOut, "WebSocket handshake timed out"};
            return to_io_error("WebSocket handshake", ec);
        }

        std::vector<HttpHeader> to_headers(const http::fields &fields) {
            std::vector<HttpHeader> out;
            for (const auto &field : fields) out.push_back({std::string(field.name_string()), std::string(field.value())});
            return out;
        }

        std::string join(const std::vector<std::string> &items, const char *separator) {
            std::string out;
            for (const auto &item : items) {
                if (!out.empty()) out += separator;
                out += item;
            }
            return out;
        }

        template <typename T>
        void attach_providers(WsSession<CoreStream> &session, const std::shared_ptr<StreamCore> &core) {
            session.peer_provider = [core] { return Result<SocketAddress>(core->peer); };
            if (core->tls_context) session.tls_provider = [core] { return Result<TlsInfo>(core->tls_details); };
        }

        // =========================================================================================
        // Server
        // =========================================================================================
        struct WsAcceptWaiter {
            Promise<Result<WebSocket>> promise;
            bool done = false;
            CancellationRegistration registration;
        };

        struct WsServerCore : std::enable_shared_from_this<WsServerCore> {
            static constexpr std::size_t kReadyCap = 64;
            static constexpr std::size_t kHandshakeCap = 128;

            Strand strand{asio::make_strand(reactor())};
            TcpListener listener;
            WebSocketServerOptions options;
            std::atomic<bool> open{true};
            std::deque<WebSocket> ready;
            std::deque<std::shared_ptr<WsAcceptWaiter>> waiters;
            std::size_t handshaking = 0;
            bool accepting = false;
            std::shared_ptr<std::atomic<std::size_t>> live = std::make_shared<std::atomic<std::size_t>>(0);

            static WebSocket make_socket(std::shared_ptr<WsOps> ops) { return WsFactory::make(std::move(ops)); }

            void pump_accept() {
                if (!open.load(std::memory_order_acquire) || accepting) return;
                if (ready.size() + handshaking >= kReadyCap || handshaking >= kHandshakeCap) return;
                accepting = true;
                auto accepted = listener.accept();
                accepted.on_done([self = shared_from_this(), accepted]() mutable {
                    asio::post(self->strand, [self, accepted]() mutable {
                        self->accepting = false;
                        Result<TcpConnection> result = std::unexpected(IoError{IoErrorCode::Closed, "listener closed"});
                        try {
                            result = accepted.wait();
                        } catch (...) {
                            return;
                        }
                        if (!self->open.load(std::memory_order_acquire)) return;
                        if (!result) {
                            if (result.error().code == IoErrorCode::Closed) return;
                            auto timer = std::make_shared<asio::steady_timer>(self->strand, std::chrono::milliseconds(50));
                            timer->async_wait([self, timer](const ErrorCode &) { self->pump_accept(); });
                            return;
                        }
                        self->begin_handshake(std::move(*result));
                        self->pump_accept();
                    });
                });
            }

            void begin_handshake(TcpConnection connection) {
                if (live->load(std::memory_order_acquire) >= options.max_connections) {
                    connection.close();
                    return;
                }
                auto core = StreamCore::take(std::move(connection));
                auto session = std::make_shared<WsSession<CoreStream>>(options.socket, CoreStream(core));
                live->fetch_add(1, std::memory_order_acq_rel);
                session->live = live;
                attach_providers<void>(*session, core);
                ++handshaking;

                auto settled = std::make_shared<bool>(false);
                auto timer = std::make_shared<asio::steady_timer>(strand, options.socket.handshake_timeout);
                timer->async_wait([session, settled](const ErrorCode &ec) {
                    if (!ec && !*settled) session->abort();
                });
                // Beast completes upgrade steps on the session's strand; the server's queues belong to the server's
                // strand, so every outcome is marshalled back before it touches shared state.
                auto finish = [self = shared_from_this(), session, settled, timer](bool accepted_ok) {
                    asio::post(self->strand, [self, session, settled, timer, accepted_ok] {
                        if (*settled) return;
                        *settled = true;
                        timer->cancel();
                        --self->handshaking;
                        if (accepted_ok) {
                            session->start();
                            self->ready.push_back(make_socket(session));
                            self->deliver();
                        } else {
                            session->abort();
                        }
                        self->pump_accept();
                    });
                };

                auto buffer = std::make_shared<beast::flat_buffer>(options.max_request_header_bytes);
                auto parser = std::make_shared<http::request_parser<http::empty_body>>();
                parser->header_limit(static_cast<std::uint32_t>(options.max_request_header_bytes));
                http::async_read(session->ws.next_layer(), *buffer, *parser,
                                 asio::bind_executor(session->executor(), [self = shared_from_this(), session, buffer, parser, finish](const ErrorCode &ec, std::size_t) mutable {
                                     if (ec) return finish(false);
                                     self->on_request(session, buffer, parser->release(), std::move(finish));
                                 }));
            }

            template <typename Finish>
            void on_request(const std::shared_ptr<WsSession<CoreStream>> &session, const std::shared_ptr<beast::flat_buffer> &,
                            http::request<http::empty_body> request, Finish finish) {
                auto reject = [&](http::status status, const char *why) {
                    auto response = std::make_shared<http::response<http::string_body>>(status, request.version());
                    response->set(http::field::server, "SturdyEngine Async WebSocket");
                    response->set(http::field::connection, "close");
                    response->set(http::field::content_type, "text/plain");
                    if (status == http::status::upgrade_required) response->set(http::field::upgrade, "websocket");
                    response->body() = why;
                    response->prepare_payload();
                    http::async_write(session->ws.next_layer(), *response, [response, finish](const ErrorCode &, std::size_t) mutable { finish(false); });
                };
                if (!websocket::is_upgrade(request)) return reject(http::status::upgrade_required, "WebSocket upgrade required");

                std::string target(request.target());
                const std::string path = target.substr(0, target.find('?'));
                if (!options.paths.empty() && std::find(options.paths.begin(), options.paths.end(), path) == options.paths.end()) {
                    return reject(http::status::not_found, "no such WebSocket endpoint");
                }
                const std::string origin(request[http::field::origin]);
                if (!options.allowed_origins.empty() && !origin.empty() &&
                    std::find(options.allowed_origins.begin(), options.allowed_origins.end(), origin) == options.allowed_origins.end()) {
                    return reject(http::status::forbidden, "origin not allowed");
                }

                // Subprotocol: the first server preference the client also offered.
                std::string chosen;
                const std::string offered_text(request[http::field::sec_websocket_protocol]);
                if (!options.subprotocols.empty() && !offered_text.empty()) {
                    std::vector<std::string> offered;
                    std::size_t start = 0;
                    while (start <= offered_text.size()) {
                        auto comma = offered_text.find(',', start);
                        if (comma == std::string::npos) comma = offered_text.size();
                        std::string item = offered_text.substr(start, comma - start);
                        item.erase(0, item.find_first_not_of(" \t"));
                        item.erase(item.find_last_not_of(" \t") + 1);
                        if (!item.empty()) offered.push_back(std::move(item));
                        start = comma + 1;
                    }
                    for (const auto &preference : options.subprotocols) {
                        if (std::find(offered.begin(), offered.end(), preference) != offered.end()) {
                            chosen = preference;
                            break;
                        }
                    }
                }
                session->ws.set_option(websocket::stream_base::decorator([chosen](websocket::response_type &response) {
                    response.set(http::field::server, "SturdyEngine Async WebSocket");
                    if (!chosen.empty()) response.set(http::field::sec_websocket_protocol, chosen);
                }));
                session->request.target = target;
                session->request.headers = to_headers(request);
                session->request.origin = origin;
                session->request.subprotocol = chosen;
                session->ws.async_accept(request, [finish, request](const ErrorCode &ec) mutable { finish(!ec); });
            }

            void deliver() {
                while (!waiters.empty()) {
                    auto w = waiters.front();
                    if (w->done) {
                        waiters.pop_front();
                        continue;
                    }
                    if (ready.empty()) break;
                    waiters.pop_front();
                    w->done = true;
                    w->registration.reset();
                    WebSocket socket = std::move(ready.front());
                    ready.pop_front();
                    (void)w->promise.set_value(Result<WebSocket>(std::move(socket)));
                }
                pump_accept();
            }

            void close_now() {
                if (!open.exchange(false, std::memory_order_acq_rel)) return;
                listener.close();
                for (auto &w : waiters) {
                    if (!w->done) {
                        w->done = true;
                        w->registration.reset();
                        (void)w->promise.set_value(Result<WebSocket>(std::unexpected(closed_error("WebSocket server closed"))));
                    }
                }
                waiters.clear();
                ready.clear();
            }
        };
    } // namespace Detail

    using namespace Detail;

    // =============================================================================================
    // WebSocket
    // =============================================================================================
    WebSocket::~WebSocket() noexcept {
        if (ops_) ops_->abort();
    }

    WebSocket &WebSocket::operator=(WebSocket &&other) noexcept {
        if (this != &other) {
            if (ops_) ops_->abort();
            ops_ = std::move(other.ops_);
        }
        return *this;
    }

    TaskHandle<std::expected<WebSocket, IoError>> WebSocket::connect(std::string url, WebSocketClientOptions options, CancellationToken cancellation) {
        auto parsed = parse_url(url, {"ws", "wss"});
        if (!parsed) return failed<WebSocket>(parsed.error());
        for (const auto &header : options.headers) {
            if (has_line_break(header.name) || has_line_break(header.value) || header.name.empty()) {
                return failed<WebSocket>(IoError{IoErrorCode::InvalidArgument, "request headers must not contain line breaks"});
            }
        }
        for (const auto &protocol : options.subprotocols) {
            if (protocol.empty() || has_line_break(protocol) || protocol.find(',') != std::string::npos) {
                return failed<WebSocket>(IoError{IoErrorCode::InvalidArgument, "invalid subprotocol name"});
            }
        }
        const auto timeout = options.timeout.count() > 0 ? options.timeout : std::chrono::milliseconds(30'000);
        auto connection = parsed->secure ? TcpConnection::connect_tls(parsed->host, parsed->port, options.tls, timeout, cancellation)
                                          : TcpConnection::connect(parsed->host, parsed->port, timeout, cancellation);

        Promise<Result<WebSocket>> promise;
        auto handle = promise.handle();
        connection.on_done([connection, promise, parsed = *parsed, options = std::move(options), cancellation]() mutable {
            Result<TcpConnection> connected = std::unexpected(IoError{IoErrorCode::Unknown, ""});
            try {
                connected = connection.wait();
            } catch (...) {
                (void)promise.set_exception(std::current_exception());
                return;
            }
            if (!connected) return (void)promise.set_value(Result<WebSocket>(std::unexpected(std::move(connected.error()))));

            auto core = StreamCore::take(std::move(*connected));
            auto session = std::make_shared<WsSession<CoreStream>>(options.socket, CoreStream(core));
            attach_providers<void>(*session, core);
            session->request.target = parsed.target;
            session->request.headers = options.headers;
            session->request.origin = {};
            const std::string protocols = join(options.subprotocols, ", ");
            session->ws.set_option(websocket::stream_base::decorator([headers = options.headers, protocols](websocket::request_type &request) {
                request.set(http::field::user_agent, "SturdyEngine Async WebSocket");
                for (const auto &header : headers) request.set(header.name, header.value);
                if (!protocols.empty()) request.set(http::field::sec_websocket_protocol, protocols);
            }));

            auto response = std::make_shared<websocket::response_type>();
            auto registration = std::make_shared<CancellationRegistration>();
            if (cancellation.can_be_cancelled()) {
                *registration = cancellation.on_cancel([weak = std::weak_ptr<WsSession<CoreStream>>(session)] {
                    if (auto s = weak.lock()) s->abort();
                });
            }
            asio::post(session->executor(), [session, response, registration, promise, parsed, cancellation]() mutable {
                session->ws.async_handshake(*response, parsed.host_header, parsed.target,
                                            [session, response, registration, promise, cancellation](const ErrorCode &ec) mutable {
                                                registration->reset();
                                                if (cancellation.is_cancelled()) {
                                                    session->abort();
                                                    return (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                                                }
                                                if (ec) {
                                                    session->abort();
                                                    return (void)promise.set_value(Result<WebSocket>(std::unexpected(handshake_error(ec))));
                                                }
                                                session->request.subprotocol = std::string((*response)[http::field::sec_websocket_protocol]);
                                                session->start();
                                                (void)promise.set_value(Result<WebSocket>(WsServerCore::make_socket(session)));
                                            });
            });
        });
        return handle;
    }

    namespace {
        const WebSocketRequest &empty_request() {
            static const WebSocketRequest request;
            return request;
        }
    } // namespace

    TaskHandle<std::expected<WebSocketMessage, IoError>> WebSocket::receive(CancellationToken cancellation) {
        if (!ops_) return failed<WebSocketMessage>(closed_error());
        return ops_->receive(std::move(cancellation));
    }
    TaskHandle<std::expected<void, IoError>> WebSocket::send_text(std::string text) {
        if (!ops_) return failed<void>(closed_error());
        std::vector<std::byte> data(text.size());
        std::memcpy(data.data(), text.data(), text.size());
        return ops_->send(true, std::move(data));
    }
    TaskHandle<std::expected<void, IoError>> WebSocket::send_binary(std::vector<std::byte> data) {
        if (!ops_) return failed<void>(closed_error());
        return ops_->send(false, std::move(data));
    }
    TaskHandle<std::expected<void, IoError>> WebSocket::ping(std::vector<std::byte> payload) {
        if (!ops_) return failed<void>(closed_error());
        return ops_->ping(std::move(payload));
    }
    TaskHandle<std::expected<void, IoError>> WebSocket::close(std::uint16_t code, std::string reason) {
        if (!ops_) return failed<void>(closed_error());
        return ops_->close(code, std::move(reason));
    }
    void WebSocket::abort() noexcept {
        if (ops_) ops_->abort();
    }
    bool WebSocket::is_open() const noexcept { return ops_ && ops_->is_open(); }
    const WebSocketRequest &WebSocket::request() const noexcept { return ops_ ? ops_->request : empty_request(); }
    std::optional<WebSocketCloseInfo> WebSocket::close_info() const { return ops_ ? ops_->close_info() : std::nullopt; }
    std::expected<SocketAddress, IoError> WebSocket::peer_address() const {
        if (!ops_) return std::unexpected(closed_error());
        return ops_->peer_address();
    }
    std::expected<TlsInfo, IoError> WebSocket::tls_info() const {
        if (!ops_) return std::unexpected(closed_error());
        return ops_->tls_info();
    }

    // =============================================================================================
    // WebSocketServer
    // =============================================================================================
    WebSocketServer::~WebSocketServer() noexcept { close(); }
    WebSocketServer &WebSocketServer::operator=(WebSocketServer &&other) noexcept {
        if (this != &other) {
            close();
            core_ = std::move(other.core_);
        }
        return *this;
    }

    TaskHandle<std::expected<WebSocketServer, IoError>> WebSocketServer::listen(WebSocketServerOptions options) {
        auto opened = options.tls ? TcpListener::listen_tls(options.bind_address, options.port, *options.tls).wait()
                                  : TcpListener::listen(options.bind_address, options.port).wait();
        Promise<Result<WebSocketServer>> promise;
        if (!opened) {
            (void)promise.set_value(Result<WebSocketServer>(std::unexpected(std::move(opened.error()))));
            return promise.handle();
        }
        auto core = std::make_shared<WsServerCore>();
        core->listener = std::move(*opened);
        core->options = std::move(options);
        WebSocketServer server;
        server.core_ = core;
        asio::post(core->strand, [core] { core->pump_accept(); });
        (void)promise.set_value(Result<WebSocketServer>(std::move(server)));
        return promise.handle();
    }

    TaskHandle<std::expected<WebSocket, IoError>> WebSocketServer::accept(CancellationToken cancellation) {
        if (!core_) return failed<WebSocket>(closed_error("WebSocket server closed"));
        if (cancellation.is_cancelled()) return cancelled_handle<WebSocket>();
        auto waiter = std::make_shared<WsAcceptWaiter>();
        auto handle = waiter->promise.handle();
        asio::post(core_->strand, [core = core_, waiter, cancellation] {
            if (!core->open.load(std::memory_order_acquire)) {
                (void)waiter->promise.set_value(Result<WebSocket>(std::unexpected(closed_error("WebSocket server closed"))));
                return;
            }
            if (cancellation.can_be_cancelled()) {
                waiter->registration = cancellation.on_cancel([weak_core = std::weak_ptr<WsServerCore>(core), weak_waiter = std::weak_ptr<WsAcceptWaiter>(waiter)] {
                    auto strong = weak_core.lock();
                    if (!strong) return;
                    asio::post(strong->strand, [strong, weak_waiter] {
                        if (auto w = weak_waiter.lock(); w && !w->done) {
                            w->done = true;
                            (void)w->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                        }
                    });
                });
            }
            core->waiters.push_back(waiter);
            core->deliver();
        });
        return handle;
    }

    std::expected<std::uint16_t, IoError> WebSocketServer::local_port() const {
        if (!core_) return std::unexpected(closed_error());
        return core_->listener.local_port();
    }
    bool WebSocketServer::is_open() const noexcept { return core_ && core_->open.load(std::memory_order_acquire); }
    void WebSocketServer::close() noexcept {
        if (core_) {
            try {
                asio::post(core_->strand, [core = core_] { core->close_now(); });
            } catch (...) {
            }
        }
    }

} // namespace SFT::Async

#else // Web: the browser WebSocket API will back this; servers cannot listen there.

namespace SFT::Async {
    namespace {
        template <typename T>
        TaskHandle<std::expected<T, IoError>> unsupported_ws() {
            Promise<std::expected<T, IoError>> promise;
            (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "WebSocket is not implemented on Web yet"}));
            return promise.handle();
        }
    } // namespace
    WebSocket::~WebSocket() noexcept {}
    WebSocket &WebSocket::operator=(WebSocket &&other) noexcept { ops_ = std::move(other.ops_); return *this; }
    TaskHandle<std::expected<WebSocket, IoError>> WebSocket::connect(std::string, WebSocketClientOptions, CancellationToken) { return unsupported_ws<WebSocket>(); }
    TaskHandle<std::expected<WebSocketMessage, IoError>> WebSocket::receive(CancellationToken) { return unsupported_ws<WebSocketMessage>(); }
    TaskHandle<std::expected<void, IoError>> WebSocket::send_text(std::string) { return unsupported_ws<void>(); }
    TaskHandle<std::expected<void, IoError>> WebSocket::send_binary(std::vector<std::byte>) { return unsupported_ws<void>(); }
    TaskHandle<std::expected<void, IoError>> WebSocket::ping(std::vector<std::byte>) { return unsupported_ws<void>(); }
    TaskHandle<std::expected<void, IoError>> WebSocket::close(std::uint16_t, std::string) { return unsupported_ws<void>(); }
    void WebSocket::abort() noexcept {}
    bool WebSocket::is_open() const noexcept { return false; }
    const WebSocketRequest &WebSocket::request() const noexcept { static const WebSocketRequest r; return r; }
    std::optional<WebSocketCloseInfo> WebSocket::close_info() const { return std::nullopt; }
    std::expected<SocketAddress, IoError> WebSocket::peer_address() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    std::expected<TlsInfo, IoError> WebSocket::tls_info() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    WebSocketServer::~WebSocketServer() noexcept {}
    WebSocketServer &WebSocketServer::operator=(WebSocketServer &&other) noexcept { core_ = std::move(other.core_); return *this; }
    TaskHandle<std::expected<WebSocketServer, IoError>> WebSocketServer::listen(WebSocketServerOptions) { return unsupported_ws<WebSocketServer>(); }
    TaskHandle<std::expected<WebSocket, IoError>> WebSocketServer::accept(CancellationToken) { return unsupported_ws<WebSocket>(); }
    std::expected<std::uint16_t, IoError> WebSocketServer::local_port() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    bool WebSocketServer::is_open() const noexcept { return false; }
    void WebSocketServer::close() noexcept {}
} // namespace SFT::Async

#endif
