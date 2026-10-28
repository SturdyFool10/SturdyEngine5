#include <Async/BindingInternal.hpp>

#include <Async/FileHandle.hpp>
#include <Async/HttpClient.hpp>
#include <Async/HttpServer.hpp>
#include <Async/Net.hpp>
#include <Async/ParIter.hpp>
#include <Async/Parallel.hpp>
#include <Async/Quic.hpp>
#include <Async/Signal.hpp>
#include <Async/Stun.hpp>
#include <Async/ThreadPool.hpp>
#include <Async/Timer.hpp>
#include <Async/WebSocket.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>

namespace SFT::Async::Binding {

    using namespace Detail;

    // =================================================================================================================
    // Op
    // =================================================================================================================
    struct Op::State {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> done{false};
        std::vector<std::function<void()>> callbacks;
        CancellationSource source;
    };

    Op::Op() : state_(std::make_shared<State>()) {}
    bool Op::done() const noexcept { return state_->done.load(std::memory_order_acquire); }
    OpStatus Op::status() const noexcept { return done() ? value_.status : OpStatus::Pending; }
    CancellationToken Op::token() const { return state_->source.token(); }
    void Op::cancel() noexcept { (void)state_->source.cancel(); }

    bool Op::wait(std::int64_t timeout_ms) {
        std::unique_lock lock(state_->mutex);
        if (timeout_ms < 0) {
            state_->cv.wait(lock, [&] { return state_->done.load(std::memory_order_acquire); });
            return true;
        }
        return state_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return state_->done.load(std::memory_order_acquire); });
    }

    void Op::on_complete(std::function<void()> fn) {
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->done.load(std::memory_order_acquire)) {
                state_->callbacks.push_back(std::move(fn));
                return;
            }
        }
        fn();
    }

    void Op::complete(Value value) {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->done.load(std::memory_order_relaxed)) return;
            value_ = std::move(value);
            if (value_.status == OpStatus::Pending) value_.status = OpStatus::Ok;
            callbacks.swap(state_->callbacks);
            state_->done.store(true, std::memory_order_release);
        }
        state_->cv.notify_all();
        for (auto &fn : callbacks) {
            try {
                fn();
            } catch (...) {
            }
        }
    }

    void Op::fail(const IoError &error) { fail(error.code, error.message); }
    void Op::fail(IoErrorCode code, std::string message) {
        Value v;
        v.status = OpStatus::Failed;
        v.code = code;
        v.error = std::move(message);
        complete(std::move(v));
    }
    void Op::mark_cancelled() {
        Value v;
        v.status = OpStatus::Cancelled;
        v.code = IoErrorCode::Closed;
        v.error = "cancelled";
        complete(std::move(v));
    }

    int wait_any(const std::vector<OpPtr> &ops, std::int64_t timeout_ms) {
        if (ops.empty()) return -1;
        struct Gate {
            std::mutex mutex;
            std::condition_variable cv;
            int winner = -1;
        };
        auto gate = std::make_shared<Gate>();
        for (std::size_t i = 0; i < ops.size(); ++i) {
            ops[i]->on_complete([gate, i] {
                {
                    std::lock_guard lock(gate->mutex);
                    if (gate->winner < 0) gate->winner = static_cast<int>(i);
                }
                gate->cv.notify_all();
            });
        }
        std::unique_lock lock(gate->mutex);
        if (timeout_ms < 0) gate->cv.wait(lock, [&] { return gate->winner >= 0; });
        else gate->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return gate->winner >= 0; });
        return gate->winner;
    }

    // =================================================================================================================
    // Capabilities, TLS helpers
    // =================================================================================================================
    std::uint32_t features() noexcept {
        std::uint32_t bits = 0;
        if (quic_available()) bits |= FeatureQuic;
#if defined(STURDY_ASYNC_HTTP2)
        bits |= FeatureHttp2;
#endif
#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
        bits |= FeatureHttp3;
#endif
        if (SFT::Async::Detail::file_uring_available()) bits |= FeatureIoUring;
#if defined(STURDY_ASYNC_PROCESS)
        bits |= FeatureProcess;
#endif
#if defined(STURDY_ASYNC_ZLIB)
        bits |= FeatureGzip;
#endif
#if defined(__linux__)
        bits |= FeatureNativeFileWatch;
#endif
        return bits;
    }

    TlsClientOptions Detail::to_options(const TlsClientSpec &spec) {
        TlsClientOptions o;
        o.server_name = spec.server_name;
        o.use_system_roots = spec.use_system_roots;
        o.ca_certificates_pem = spec.ca_pem;
        o.pinned_sha256 = spec.pinned_sha256;
        o.identify_peer_by_certificate_only = spec.identify_peer_by_certificate_only;
        o.client_certificate_chain_pem = spec.client_certificate_pem;
        o.client_private_key_pem = spec.client_private_key_pem;
        o.alpn = spec.alpn;
        o.handshake_timeout_ms = spec.handshake_timeout_ms;
        return o;
    }
    TlsServerOptions Detail::to_options(const TlsServerSpec &spec) {
        TlsServerOptions o;
        o.certificate_chain_pem = spec.certificate_pem;
        o.private_key_pem = spec.private_key_pem;
        o.client_ca_pem = spec.client_ca_pem;
        o.require_client_certificate = spec.require_client_certificate;
        o.accept_any_client_certificate = spec.accept_any_client_certificate;
        o.pinned_client_sha256 = spec.pinned_client_sha256;
        o.alpn = spec.alpn;
        o.handshake_timeout_ms = spec.handshake_timeout_ms;
        return o;
    }
    Fields Detail::tls_fields(const TlsInfo &info) {
        return {{"protocol", info.protocol}, {"cipher", info.cipher}, {"alpn", info.alpn}, {"peer_sha256", info.peer_sha256}};
    }

    Identity generate_identity(const std::string &common_name, unsigned valid_days) {
        auto generated = SFT::Async::generate_self_signed_identity(common_name, valid_days);
        if (!generated) throw BindingError(generated.error().code, generated.error().message);
        auto digest = SFT::Async::certificate_sha256(generated->certificate_pem);
        return Identity{generated->certificate_pem, generated->private_key_pem, digest ? *digest : std::string{}};
    }
    std::string certificate_sha256(const std::string &certificate_pem) {
        auto digest = SFT::Async::certificate_sha256(certificate_pem);
        if (!digest) throw BindingError(digest.error().code, digest.error().message);
        return *digest;
    }

    const char *object_kind(const ObjectPtr &object) noexcept { return object ? object->kind() : "null"; }

    // =================================================================================================================
    // Timers, signals, DNS, STUN
    // =================================================================================================================
    OpPtr sleep(std::int64_t ms_) {
        return launch([&](CancellationToken token) { return SFT::Async::sleep_for(std::chrono::milliseconds(ms_ < 0 ? 0 : ms_), std::move(token)); }, [](Value &) {});
    }
    OpPtr shutdown_signal() {
        return launch([](CancellationToken token) { return SFT::Async::shutdown_signal(std::move(token)); },
                      [](int signal, Value &out) { out.number = signal; });
    }
    OpPtr resolve_host(const std::string &host) {
        return launch([&](CancellationToken) { return SFT::Async::resolve_host(host); }, [](std::vector<SocketAddress> &&addresses, Value &out) {
            for (auto &a : addresses) out.fields.emplace_back("address", a.host);
        });
    }
    OpPtr stun_query(const std::string &server, std::uint16_t port, std::int64_t timeout_ms) {
        return launch([&](CancellationToken token) { return SFT::Async::stun_query(server, port, ms(timeout_ms), std::move(token)); },
                      [](SocketAddress &&address, Value &out) {
                          out.text = address.host;
                          out.number = address.port;
                      });
    }

    // =================================================================================================================
    // TCP
    // =================================================================================================================
    namespace {
        constexpr const char *kTcp = "tcp";
        constexpr const char *kTcpListener = "tcp_listener";
        constexpr const char *kUdp = "udp";
        auto tcp_object = [](TcpConnection &&c, Value &out) { out.object = make_object<TcpConnection>(kTcp, std::move(c)); };
    } // namespace

    OpPtr tcp_connect(const std::string &host, std::uint16_t port, const TlsClientSpec *tls, std::int64_t timeout_ms) {
        const std::optional<TlsClientOptions> options = tls ? std::optional<TlsClientOptions>(to_options(*tls)) : std::nullopt;
        return launch(
            [&](CancellationToken token) {
                return options ? TcpConnection::connect_tls(host, port, *options, ms(timeout_ms), std::move(token))
                               : TcpConnection::connect(host, port, ms(timeout_ms), std::move(token));
            },
            tcp_object);
    }
    OpPtr tcp_listen(const std::string &host, std::uint16_t port, const TlsServerSpec *tls, std::int32_t backlog) {
        const std::optional<TlsServerOptions> options = tls ? std::optional<TlsServerOptions>(to_options(*tls)) : std::nullopt;
        const int queue = backlog > 0 ? backlog : 1024;
        return launch([&](CancellationToken) { return options ? TcpListener::listen_tls(host, port, *options, queue) : TcpListener::listen(host, port, queue); },
                      [](TcpListener &&l, Value &out) { out.object = make_object<TcpListener>(kTcpListener, std::move(l)); });
    }
    OpPtr tcp_accept(const ObjectPtr &listener) {
        auto &l = get<TcpListener>(listener, kTcpListener);
        return launch([&, listener](CancellationToken token) { return l.accept(std::move(token)); }, tcp_object);
    }
    std::uint16_t tcp_listener_port(const ObjectPtr &listener) {
        auto port = get<TcpListener>(listener, kTcpListener).local_port();
        if (!port) throw BindingError(port.error().code, port.error().message);
        return *port;
    }
    std::string tcp_listener_address(const ObjectPtr &listener) {
        auto address = get<TcpListener>(listener, kTcpListener).local_address();
        if (!address) throw BindingError(address.error().code, address.error().message);
        return address->host + ":" + std::to_string(address->port);
    }
    void tcp_listener_close(const ObjectPtr &listener) { get<TcpListener>(listener, kTcpListener).close(); }
    bool tcp_listener_is_open(const ObjectPtr &listener) { return get<TcpListener>(listener, kTcpListener).is_open(); }

    OpPtr tcp_send(const ObjectPtr &connection, Bytes data) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection, data = std::move(data)](CancellationToken token) mutable { return c.send(to_native(data), std::move(token)); },
                      [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr tcp_receive(const ObjectPtr &connection, std::size_t max_bytes) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken token) { return c.receive(max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&data, Value &out) { out.bytes = from_native(data); });
    }
    OpPtr tcp_receive_exact(const ObjectPtr &connection, std::size_t count) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken token) { return c.receive_exact(count, std::move(token)); },
                      [](std::vector<std::byte> &&data, Value &out) { out.bytes = from_native(data); });
    }
    OpPtr tcp_receive_until(const ObjectPtr &connection, const std::string &delimiter, std::size_t max_bytes) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken token) { return c.receive_until(delimiter, max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&data, Value &out) { out.bytes = from_native(data); });
    }
    OpPtr tcp_shutdown_write(const ObjectPtr &connection) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken) { return c.shutdown_write(); }, [](Value &) {});
    }
    OpPtr tcp_start_tls_client(const ObjectPtr &connection, const std::string &host, const TlsClientSpec &tls) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken token) { return c.start_tls_client(host, to_options(tls), std::move(token)); }, [](Value &) {});
    }
    OpPtr tcp_start_tls_server(const ObjectPtr &connection, const TlsServerSpec &tls) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken) { return c.start_tls_server(to_options(tls)); }, [](Value &) {});
    }
    void tcp_close(const ObjectPtr &connection) { get<TcpConnection>(connection, kTcp).close(); }
    bool tcp_is_open(const ObjectPtr &connection) { return get<TcpConnection>(connection, kTcp).is_open(); }
    Fields tcp_info(const ObjectPtr &connection) {
        auto &c = get<TcpConnection>(connection, kTcp);
        Fields fields;
        if (auto peer = c.peer_address()) fields.emplace_back("peer", peer->host + ":" + std::to_string(peer->port));
        if (auto local = c.local_address()) fields.emplace_back("local", local->host + ":" + std::to_string(local->port));
        if (c.is_tls()) {
            if (auto tls = c.tls_info()) {
                auto more = tls_fields(*tls);
                fields.insert(fields.end(), more.begin(), more.end());
            }
        }
        return fields;
    }
    OpPtr tcp_set_nodelay(const ObjectPtr &connection, bool enabled) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken) { return c.set_nodelay(enabled); }, [](Value &) {});
    }
    OpPtr tcp_set_keepalive(const ObjectPtr &connection, bool enabled) {
        auto &c = get<TcpConnection>(connection, kTcp);
        return launch([&, connection](CancellationToken) { return c.set_keepalive(enabled); }, [](Value &) {});
    }

    // =================================================================================================================
    // UDP
    // =================================================================================================================
    OpPtr udp_bind(const std::string &host, std::uint16_t port, const UdpSpec &spec) {
        UdpOptions options;
        options.reuse_address = spec.reuse_address;
        options.broadcast = spec.broadcast;
        options.receive_buffer = static_cast<std::size_t>(spec.receive_buffer);
        options.send_buffer = static_cast<std::size_t>(spec.send_buffer);
        return launch([&](CancellationToken) { return UdpSocket::bind(host, port, options); },
                      [](UdpSocket &&s, Value &out) { out.object = make_object<UdpSocket>(kUdp, std::move(s)); });
    }
    OpPtr udp_connect(const ObjectPtr &socket, const std::string &host, std::uint16_t port) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.connect(host, port); }, [](Value &) {});
    }
    OpPtr udp_send(const ObjectPtr &socket, Bytes data) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.send(to_native(data)); }, [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr udp_send_to(const ObjectPtr &socket, const std::string &host, std::uint16_t port, Bytes data) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.send_to(host, port, to_native(data)); },
                      [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr udp_receive(const ObjectPtr &socket, std::size_t max_bytes) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken token) { return s.receive_from(max_bytes, std::move(token)); }, [](UdpDatagram &&d, Value &out) {
            out.bytes = from_native(d.data);
            out.text = d.sender_host;
            out.number = d.sender_port;
        });
    }
    std::uint16_t udp_port(const ObjectPtr &socket) {
        auto port = get<UdpSocket>(socket, kUdp).local_port();
        if (!port) throw BindingError(port.error().code, port.error().message);
        return *port;
    }
    std::string udp_address(const ObjectPtr &socket) {
        auto address = get<UdpSocket>(socket, kUdp).local_address();
        if (!address) throw BindingError(address.error().code, address.error().message);
        return address->host + ":" + std::to_string(address->port);
    }
    OpPtr udp_set_broadcast(const ObjectPtr &socket, bool enabled) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.set_broadcast(enabled); }, [](Value &) {});
    }
    OpPtr udp_join_multicast(const ObjectPtr &socket, const std::string &group, const std::string &interface_address) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.join_multicast(group, interface_address); }, [](Value &) {});
    }
    OpPtr udp_leave_multicast(const ObjectPtr &socket, const std::string &group, const std::string &interface_address) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.leave_multicast(group, interface_address); }, [](Value &) {});
    }
    OpPtr udp_set_multicast_loopback(const ObjectPtr &socket, bool enabled) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.set_multicast_loopback(enabled); }, [](Value &) {});
    }
    OpPtr udp_set_multicast_ttl(const ObjectPtr &socket, std::uint32_t hops) {
        auto &s = get<UdpSocket>(socket, kUdp);
        return launch([&, socket](CancellationToken) { return s.set_multicast_ttl(hops); }, [](Value &) {});
    }
    void udp_close(const ObjectPtr &socket) { get<UdpSocket>(socket, kUdp).close(); }
    bool udp_is_open(const ObjectPtr &socket) { return get<UdpSocket>(socket, kUdp).is_open(); }

    // =================================================================================================================
    // HTTP client / server
    // =================================================================================================================
    namespace {
        constexpr const char *kHttpClient = "http_client";
    } // namespace

    ObjectPtr http_client_new(const HttpClientSpec &spec) {
        HttpClientOptions o;
        o.tls = to_options(spec.tls);
        o.timeout = ms(spec.timeout_ms);
        o.max_redirects = spec.max_redirects;
        o.max_response_body_bytes = spec.max_response_body_bytes;
        o.keep_alive = spec.keep_alive;
        o.decompress = spec.decompress;
        o.cookies = spec.cookies;
        o.http2 = spec.http2;
        o.http3 = spec.http3;
        o.http3_prior_knowledge = spec.http3_prior_knowledge;
        o.user_agent = spec.user_agent;
        o.proxy = spec.proxy;
        o.no_proxy = spec.no_proxy;
        o.allow_https_downgrade = spec.allow_https_downgrade;
        o.max_response_header_bytes = spec.max_response_header_bytes;
        o.max_idle_connections_per_host = spec.max_idle_connections_per_host;
        o.idle_connection_timeout = ms(spec.idle_connection_timeout_ms);
        o.http3_connect_timeout = ms(spec.http3_connect_timeout_ms);
        if (spec.cookie_jar) {
            o.cookie_jar = get<std::shared_ptr<CookieJar>>(spec.cookie_jar, kCookieJar);
            o.cookies = true;
        }
        return make_object<HttpClient>(kHttpClient, HttpClient(std::move(o)));
    }
    ObjectPtr http_client_cookie_jar(const ObjectPtr &client) {
        auto jar = get<HttpClient>(client, kHttpClient).cookie_jar();
        return jar ? make_object<std::shared_ptr<CookieJar>>(kCookieJar, std::move(jar)) : ObjectPtr{};
    }
    OpPtr http_request(const ObjectPtr &client, const std::string &method, const std::string &url, const Fields &headers, Bytes body) {
        auto &c = get<HttpClient>(client, kHttpClient);
        return launch(
            [&, client](CancellationToken token) { return c.request(method, url, to_headers(headers), to_string_bytes(body), std::move(token)); },
            [](HttpResponseMessage &&r, Value &out) {
                out.number = r.status;
                out.fields = from_headers(r.headers);
                out.bytes.assign(r.body.begin(), r.body.end());
                out.text = r.final_url;
            });
    }
    void http_client_close_idle(const ObjectPtr &client) { get<HttpClient>(client, kHttpClient).close_idle(); }

    // =================================================================================================================
    // WebSocket
    // =================================================================================================================
    namespace {
        constexpr const char *kWsServer = "ws_server";
        WebSocketOptions to_options(const WebSocketSpec &spec) {
            WebSocketOptions o;
            o.max_message_bytes = spec.max_message_bytes;
            o.max_queued_send_bytes = spec.max_queued_send_bytes;
            o.inbox_messages = spec.inbox_messages;
            o.handshake_timeout = ms(spec.handshake_timeout_ms);
            o.idle_timeout = ms(spec.idle_timeout_ms);
            o.compression = spec.compression;
            return o;
        }
        auto ws_object = [](WebSocket &&s, Value &out) { out.object = make_object<WebSocket>(kWebSocket, std::move(s)); };
    } // namespace

    OpPtr websocket_connect(const std::string &url, const WebSocketClientSpec &spec) {
        WebSocketClientOptions o;
        o.socket = to_options(spec.socket);
        o.tls = to_options(spec.tls);
        o.headers = to_headers(spec.headers);
        o.subprotocols = spec.subprotocols;
        o.timeout = ms(spec.timeout_ms);
        return launch([&](CancellationToken token) { return WebSocket::connect(url, o, std::move(token)); }, ws_object);
    }
    OpPtr websocket_server_listen(const WebSocketServerSpec &spec) {
        WebSocketServerOptions o;
        o.bind_address = spec.bind_address;
        o.port = spec.port;
        o.socket = to_options(spec.socket);
        if (spec.use_tls) o.tls = to_options(spec.tls);
        o.allowed_origins = spec.allowed_origins;
        o.paths = spec.paths;
        o.subprotocols = spec.subprotocols;
        o.max_connections = spec.max_connections;
        o.max_request_header_bytes = spec.max_request_header_bytes;
        return launch([&](CancellationToken) { return WebSocketServer::listen(o); },
                      [](WebSocketServer &&s, Value &out) { out.object = make_object<WebSocketServer>(kWsServer, std::move(s)); });
    }
    OpPtr websocket_accept(const ObjectPtr &server) {
        auto &s = get<WebSocketServer>(server, kWsServer);
        return launch([&, server](CancellationToken token) { return s.accept(std::move(token)); }, ws_object);
    }
    std::uint16_t websocket_server_port(const ObjectPtr &server) {
        auto port = get<WebSocketServer>(server, kWsServer).local_port();
        if (!port) throw BindingError(port.error().code, port.error().message);
        return *port;
    }
    void websocket_server_close(const ObjectPtr &server) { get<WebSocketServer>(server, kWsServer).close(); }
    OpPtr websocket_send_text(const ObjectPtr &socket, std::string text) {
        auto &s = get<WebSocket>(socket, kWebSocket);
        return launch([&, socket, text = std::move(text)](CancellationToken) mutable { return s.send_text(std::move(text)); }, [](Value &) {});
    }
    OpPtr websocket_send_binary(const ObjectPtr &socket, Bytes data) {
        auto &s = get<WebSocket>(socket, kWebSocket);
        return launch([&, socket, data = std::move(data)](CancellationToken) mutable { return s.send_binary(to_native(data)); }, [](Value &) {});
    }
    OpPtr websocket_ping(const ObjectPtr &socket, Bytes payload) {
        auto &s = get<WebSocket>(socket, kWebSocket);
        return launch([&, socket, payload = std::move(payload)](CancellationToken) mutable { return s.ping(to_native(payload)); }, [](Value &) {});
    }
    OpPtr websocket_receive(const ObjectPtr &socket) {
        auto &s = get<WebSocket>(socket, kWebSocket);
        return launch([&, socket](CancellationToken token) { return s.receive(std::move(token)); }, [](WebSocketMessage &&m, Value &out) {
            out.bytes = from_native(m.data);
            out.number = m.is_text ? 1 : 0;
        });
    }
    OpPtr websocket_close(const ObjectPtr &socket, std::uint16_t code, std::string reason) {
        auto &s = get<WebSocket>(socket, kWebSocket);
        return launch([&, socket, reason = std::move(reason)](CancellationToken) mutable { return s.close(code, std::move(reason)); }, [](Value &) {});
    }
    bool websocket_is_open(const ObjectPtr &socket) { return get<WebSocket>(socket, kWebSocket).is_open(); }
    void websocket_abort(const ObjectPtr &socket) { get<WebSocket>(socket, kWebSocket).abort(); }
    Fields websocket_info(const ObjectPtr &socket) {
        auto &s = get<WebSocket>(socket, kWebSocket);
        const auto &r = s.request();
        Fields fields{{"target", r.target}, {"origin", r.origin}, {"subprotocol", r.subprotocol}};
        for (const auto &header : r.headers) fields.emplace_back("header:" + header.name, header.value);
        if (auto peer = s.peer_address()) fields.emplace_back("peer", peer->host + ":" + std::to_string(peer->port));
        if (auto tls = s.tls_info()) {
            auto more = tls_fields(*tls);
            fields.insert(fields.end(), more.begin(), more.end());
        }
        if (auto close = s.close_info()) {
            fields.emplace_back("close_code", std::to_string(close->code));
            fields.emplace_back("close_reason", close->reason);
        }
        return fields;
    }

    // =================================================================================================================
    // Data parallelism and pools
    // =================================================================================================================
    namespace {
        constexpr const char *kPool = "pool";
    }

    void par_for(std::uint64_t count, std::uint64_t min_chunk, const std::function<void(std::uint64_t)> &body) {
        auto range = SFT::Async::par_range(static_cast<usize>(count));
        if (min_chunk > 1) SFT::Async::par_range(static_cast<usize>(count)).with_min_len(static_cast<usize>(min_chunk)).for_each([&](usize i) { body(i); });
        else range.for_each([&](usize i) { body(i); });
    }
    void par_for_chunks(std::uint64_t count, std::uint64_t chunk, const std::function<void(std::uint64_t, std::uint64_t)> &body) {
        if (chunk == 0) throw BindingError(IoErrorCode::InvalidArgument, "chunk must be positive");
        const std::uint64_t pieces = (count + chunk - 1) / chunk;
        SFT::Async::par_range(static_cast<usize>(pieces)).for_each([&](usize piece) {
            const std::uint64_t begin = piece * chunk;
            body(begin, (std::min)(count, begin + chunk));
        });
    }

    void par_sort(Numeric type, void *data, std::uint64_t count) {
        if (data == nullptr && count != 0) throw BindingError(IoErrorCode::InvalidArgument, "null data");
        auto sort = [&]<typename T>() { SFT::Async::par_sort(std::span<T>(static_cast<T *>(data), static_cast<std::size_t>(count))); };
        switch (type) {
            case Numeric::F32: sort.operator()<float>(); break;
            case Numeric::F64: sort.operator()<double>(); break;
            case Numeric::I32: sort.operator()<std::int32_t>(); break;
            case Numeric::I64: sort.operator()<std::int64_t>(); break;
            case Numeric::U32: sort.operator()<std::uint32_t>(); break;
            case Numeric::U64: sort.operator()<std::uint64_t>(); break;
        }
    }
    double par_sum_f64(const double *data, std::uint64_t count) { return SFT::Async::par_iter(std::span<const double>(data, count)).sum(); }
    float par_sum_f32(const float *data, std::uint64_t count) { return SFT::Async::par_iter(std::span<const float>(data, count)).sum(); }
    std::int64_t par_sum_i64(const std::int64_t *data, std::uint64_t count) { return SFT::Async::par_iter(std::span<const std::int64_t>(data, count)).sum(); }
    std::uint64_t par_sum_u64(const std::uint64_t *data, std::uint64_t count) { return SFT::Async::par_iter(std::span<const std::uint64_t>(data, count)).sum(); }

    namespace {
        template <typename T>
        bool min_max(const T *data, std::uint64_t count, T *min, T *max) {
            if (count == 0) return false;
            std::span<const T> view(data, count);
            const auto low = SFT::Async::par_iter(view).min();
            const auto high = SFT::Async::par_iter(view).max();
            if (!low || !high) return false;
            if (min) *min = *low;
            if (max) *max = *high;
            return true;
        }
    } // namespace
    bool par_min_max_f64(const double *data, std::uint64_t count, double *min, double *max) { return min_max<double>(data, count, min, max); }
    bool par_min_max_f32(const float *data, std::uint64_t count, float *min, float *max) { return min_max<float>(data, count, min, max); }
    bool par_min_max_i64(const std::int64_t *data, std::uint64_t count, std::int64_t *min, std::int64_t *max) { return min_max<std::int64_t>(data, count, min, max); }

    ObjectPtr pool_create(std::uint32_t threads, const std::string &name) {
        ThreadPoolOptions options;
        options.threads = threads;
        options.name = name.empty() ? "pool" : name;
        return make_object<std::unique_ptr<ThreadPool>>(kPool, std::make_unique<ThreadPool>(options));
    }
    std::uint32_t pool_threads(const ObjectPtr &pool) { return static_cast<std::uint32_t>(get<std::unique_ptr<ThreadPool>>(pool, kPool)->num_threads()); }
    void pool_par_for(const ObjectPtr &pool, std::uint64_t count, std::uint64_t min_chunk, const std::function<void(std::uint64_t)> &body) {
        get<std::unique_ptr<ThreadPool>>(pool, kPool)->install([&] { par_for(count, min_chunk, body); });
    }
    void pool_install(const ObjectPtr &pool, const std::function<void()> &body) { get<std::unique_ptr<ThreadPool>>(pool, kPool)->install([&] { body(); }); }

} // namespace SFT::Async::Binding
