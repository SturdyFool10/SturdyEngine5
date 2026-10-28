#include <Async/HttpClient.hpp>
#include <Async/Promise.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <Async/BeastStream.hpp>
#include <Async/Http2Engine.hpp>
#include <Async/Http3Engine.hpp>
#include <Async/UrlInternal.hpp>

#include <boost/beast/http.hpp>

#include <charconv>
#include <deque>
#include <map>
#include <optional>

#if defined(STURDY_ASYNC_ZLIB)
#include <zlib.h>
#endif

namespace SFT::Async::Detail {
    namespace beast = boost::beast;
    namespace http = beast::http;

    struct ProxyConfig {
        bool active = false;
        std::string host;
        std::uint16_t port = 0;
        std::string authorization; ///< "Basic ..." or empty
    };

    inline std::string base64(std::string_view input) {
        static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        std::size_t i = 0;
        for (; i + 2 < input.size(); i += 3) {
            const unsigned v = (static_cast<unsigned char>(input[i]) << 16) | (static_cast<unsigned char>(input[i + 1]) << 8) | static_cast<unsigned char>(input[i + 2]);
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += table[(v >> 6) & 63];
            out += table[v & 63];
        }
        if (i + 1 == input.size()) {
            const unsigned v = static_cast<unsigned char>(input[i]) << 16;
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += "==";
        } else if (i + 2 == input.size()) {
            const unsigned v = (static_cast<unsigned char>(input[i]) << 16) | (static_cast<unsigned char>(input[i + 1]) << 8);
            out += table[(v >> 18) & 63];
            out += table[(v >> 12) & 63];
            out += table[(v >> 6) & 63];
            out += '=';
        }
        return out;
    }

    /// `http://[user:password@]host[:port]` -> ProxyConfig; an empty string means "no proxy".
    inline Result<ProxyConfig> parse_proxy(const std::string &text) {
        ProxyConfig config;
        if (text.empty()) return config;
        if (text.rfind("http://", 0) != 0) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "the proxy URL must start with http://"});
        std::string rest = text.substr(7);
        if (const auto slash = rest.find('/'); slash != std::string::npos) rest.erase(slash);
        if (const auto at = rest.rfind('@'); at != std::string::npos) {
            config.authorization = "Basic " + base64(rest.substr(0, at));
            rest.erase(0, at + 1);
        }
        std::string host = rest;
        unsigned long port = 80;
        if (!rest.empty() && rest.front() == '[') {
            const auto close = rest.find(']');
            if (close == std::string::npos) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "malformed proxy host"});
            host = rest.substr(1, close - 1);
            if (close + 2 < rest.size() && rest[close + 1] == ':') port = std::strtoul(rest.c_str() + close + 2, nullptr, 10);
        } else if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
            host = rest.substr(0, colon);
            port = std::strtoul(rest.c_str() + colon + 1, nullptr, 10);
        }
        if (host.empty() || port == 0 || port > 65535) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "the proxy needs a host and a valid port"});
        config.active = true;
        config.host = host;
        config.port = static_cast<std::uint16_t>(port);
        return config;
    }

    inline bool bypasses_proxy(const std::vector<std::string> &patterns, const std::string &host) {
        for (const std::string &pattern : patterns) {
            if (pattern == host) return true;
            if (!pattern.empty() && pattern.front() == '.' && host.size() > pattern.size() &&
                host.compare(host.size() - pattern.size(), pattern.size(), pattern) == 0) return true;
        }
        return false;
    }

    /// gzip / zlib / raw-deflate -> bytes, never producing more than `limit` (the decompression-bomb guard).
    inline Result<std::string> inflate_body(const std::string &input, std::uint64_t limit) {
#if defined(STURDY_ASYNC_ZLIB)
        auto attempt = [&](int window_bits) -> Result<std::string> {
            z_stream stream{};
            if (inflateInit2(&stream, window_bits) != Z_OK) return std::unexpected(IoError{IoErrorCode::Unknown, "zlib init failed"});
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
            stream.avail_in = static_cast<uInt>(input.size());
            std::string out;
            std::vector<char> chunk(64 * 1024);
            int status = Z_OK;
            while (status == Z_OK) {
                stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
                stream.avail_out = static_cast<uInt>(chunk.size());
                status = inflate(&stream, Z_NO_FLUSH);
                const std::size_t produced = chunk.size() - stream.avail_out;
                if (out.size() + produced > limit) {
                    inflateEnd(&stream);
                    return std::unexpected(IoError{IoErrorCode::LimitExceeded, "decompressed response exceeds the configured limit"});
                }
                out.append(chunk.data(), produced);
                if (status == Z_BUF_ERROR && stream.avail_in == 0) break;
            }
            inflateEnd(&stream);
            if (status != Z_STREAM_END) return std::unexpected(IoError{IoErrorCode::ProtocolError, "corrupt compressed response body"});
            return out;
        };
        auto framed = attempt(15 + 32); // zlib or gzip, auto-detected
        if (framed || framed.error().code == IoErrorCode::LimitExceeded) return framed;
        return attempt(-15);            // some servers send raw deflate under "Content-Encoding: deflate"
#else
        (void)input;
        (void)limit;
        return std::unexpected(IoError{IoErrorCode::Unsupported, "this build has no zlib"});
#endif
    }

#if defined(STURDY_ASYNC_ZLIB)
    inline constexpr bool kHaveZlib = true;
#else
    inline constexpr bool kHaveZlib = false;
#endif

    /// CONNECT tunnel through an HTTP proxy, then TLS to the real target over it.
    inline TaskHandle<Result<TcpConnection>> open_tunnel(ProxyConfig proxy, ParsedUrl target, TlsClientOptions tls, std::chrono::milliseconds timeout) {
        auto connected = co_await TcpConnection::connect(proxy.host, proxy.port, timeout);
        if (!connected) co_return std::unexpected(connected.error());
        TcpConnection connection = std::move(*connected);
        const std::string authority = target.host.find(':') != std::string::npos ? "[" + target.host + "]:" + std::to_string(target.port)
                                                                                 : target.host + ":" + std::to_string(target.port);
        std::string request = "CONNECT " + authority + " HTTP/1.1\r\nHost: " + authority + "\r\n";
        if (!proxy.authorization.empty()) request += "Proxy-Authorization: " + proxy.authorization + "\r\n";
        request += "\r\n";
        std::vector<std::byte> bytes(request.size());
        std::memcpy(bytes.data(), request.data(), request.size());
        auto sent = co_await connection.send_all(std::move(bytes));
        if (!sent) co_return std::unexpected(sent.error());
        auto head = co_await connection.receive_until("\r\n\r\n", 16 * 1024);
        if (!head) co_return std::unexpected(head.error());
        const std::string text(reinterpret_cast<const char *>(head->data()), head->size());
        const auto line_end = text.find("\r\n");
        const std::string status_line = text.substr(0, line_end);
        if (status_line.size() < 12 || status_line.rfind("HTTP/1.", 0) != 0 || status_line.substr(9, 3) != "200") {
            co_return std::unexpected(IoError{IoErrorCode::ConnectionRefused, "proxy refused CONNECT: " + status_line});
        }
        auto secured = co_await connection.start_tls_client(target.host, std::move(tls));
        if (!secured) co_return std::unexpected(secured.error());
        co_return Result<TcpConnection>(std::move(connection));
    }

    struct Idle {
        std::shared_ptr<StreamCore> core;
        std::chrono::steady_clock::time_point since;
    };

#if defined(STURDY_ASYNC_HTTP2)
    /// Runs an HTTP/2 engine over a reactor connection that already finished its TLS handshake with ALPN "h2".
    class CoreH2Transport final : public H2Transport, public std::enable_shared_from_this<CoreH2Transport> {
      public:
        explicit CoreH2Transport(std::shared_ptr<StreamCore> core) : core_(core), stream_(std::move(core)) {}
        [[nodiscard]] asio::any_io_executor executor() override { return core_->strand; }
        void read_some(std::function<void(bool, const std::byte *, std::size_t)> done) override {
            stream_.async_read_some(asio::buffer(buffer_), [self = shared_from_this(), done = std::move(done)](const ErrorCode &ec, std::size_t n) {
                done(!ec, reinterpret_cast<const std::byte *>(self->buffer_.data()), n);
            });
        }
        void write(std::vector<std::byte> data, std::function<void(bool)> done) override {
            auto held = std::make_shared<std::vector<std::byte>>(std::move(data));
            asio::async_write(stream_, asio::buffer(*held), [self = shared_from_this(), held, done = std::move(done)](const ErrorCode &ec, std::size_t) { done(!ec); });
        }
        void close() override { core_->request_close(); }

      private:
        std::shared_ptr<StreamCore> core_;
        CoreStream stream_;
        std::array<char, 16 * 1024> buffer_{};
    };
#endif

    struct HttpClientCore {
        explicit HttpClientCore(HttpClientOptions o) : options(std::move(o)) {
            if (auto parsed = parse_proxy(options.proxy)) proxy = *parsed;
            if (options.cookies) jar = options.cookie_jar ? options.cookie_jar : std::make_shared<CookieJar>();
#if defined(STURDY_ASYNC_HTTP2)
            if (options.http2 && options.tls.alpn.empty()) options.tls.alpn = {"h2", "http/1.1"};
#endif
        }
        ProxyConfig proxy;
        std::shared_ptr<CookieJar> jar;
        HttpClientOptions options;
        Mutex<std::map<std::string, std::deque<Idle>>> pool;
#if defined(STURDY_ASYNC_HTTP2)
        Mutex<std::map<std::string, std::shared_ptr<H2Engine>>> h2_pool;

        std::shared_ptr<H2Engine> h2_for(const std::string &key) {
            auto guard = h2_pool.lock();
            auto it = guard->find(key);
            if (it == guard->end()) return nullptr;
            if (it->second->usable()) return it->second;
            guard->erase(it);
            return nullptr;
        }
        void h2_register(const std::string &key, std::shared_ptr<H2Engine> engine) {
            auto guard = h2_pool.lock();
            auto &slot = (*guard)[key];
            if (!slot || !slot->usable()) slot = std::move(engine);
        }
#endif

#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
        struct AltService {
            std::uint16_t port = 0;
            std::chrono::steady_clock::time_point expires;
        };
        Mutex<std::map<std::string, std::shared_ptr<H3Client>>> h3_pool;
        Mutex<std::map<std::string, AltService>> alt_services;
        Mutex<std::map<std::string, std::chrono::steady_clock::time_point>> h3_broken; ///< origin -> do not try QUIC again before

        std::shared_ptr<H3Client> h3_for(const std::string &key) {
            auto guard = h3_pool.lock();
            auto it = guard->find(key);
            if (it == guard->end()) return nullptr;
            if (it->second->usable()) return it->second;
            guard->erase(it);
            return nullptr;
        }
        void h3_register(const std::string &key, std::shared_ptr<H3Client> client) {
            auto guard = h3_pool.lock();
            auto &slot = (*guard)[key];
            if (!slot || !slot->usable()) slot = std::move(client);
        }
        void mark_h3_broken(const std::string &key) {
            auto guard = h3_broken.lock();
            if (guard->size() >= 256) guard->clear();
            (*guard)[key] = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        }
        [[nodiscard]] bool h3_is_broken(const std::string &key) {
            auto guard = h3_broken.lock();
            auto it = guard->find(key);
            if (it == guard->end()) return false;
            if (it->second > std::chrono::steady_clock::now()) return true;
            guard->erase(it);
            return false;
        }
        /// Remembers (or forgets) what an `Alt-Svc` header says about this origin. Only an h3 alternative on the origin's own
        /// host is honoured: the QUIC handshake is verified against the origin's name regardless, so nothing else is gained.
        void learn_alt_svc(const std::string &key, const std::string &host, std::string_view value) {
            auto guard = alt_services.lock();
            if (value == "clear") {
                guard->erase(key);
                return;
            }
            std::size_t at = 0;
            while (at < value.size()) {
                const std::size_t entry_end = value.find(',', at);
                std::string_view entry = value.substr(at, entry_end == std::string_view::npos ? std::string_view::npos : entry_end - at);
                at = entry_end == std::string_view::npos ? value.size() : entry_end + 1;
                while (!entry.empty() && entry.front() == ' ') entry.remove_prefix(1);
                if (entry.rfind("h3=\"", 0) != 0) continue;
                const std::size_t close = entry.find('"', 4);
                if (close == std::string_view::npos) continue;
                const std::string_view authority = entry.substr(4, close - 4); // "host:port" or ":port"
                const std::size_t colon = authority.rfind(':');
                if (colon == std::string_view::npos) continue;
                if (colon != 0 && authority.substr(0, colon) != host) continue;
                unsigned port = 0;
                const auto [end, ec] = std::from_chars(authority.data() + colon + 1, authority.data() + authority.size(), port);
                if (ec != std::errc{} || end != authority.data() + authority.size() || port == 0 || port > 65535) continue;
                long max_age = 86400; // RFC 7838 default
                if (const std::size_t ma = entry.find("ma=", close); ma != std::string_view::npos) {
                    max_age = std::strtol(std::string(entry.substr(ma + 3)).c_str(), nullptr, 10);
                }
                if (guard->size() >= 256) guard->clear();
                if (max_age <= 0) guard->erase(key);
                else (*guard)[key] = {static_cast<std::uint16_t>(port), std::chrono::steady_clock::now() + std::chrono::seconds(max_age)};
                return;
            }
        }
        [[nodiscard]] std::optional<std::uint16_t> alt_service_port(const std::string &key) {
            auto guard = alt_services.lock();
            auto it = guard->find(key);
            if (it == guard->end()) return std::nullopt;
            if (it->second.expires <= std::chrono::steady_clock::now()) {
                guard->erase(it);
                return std::nullopt;
            }
            return it->second.port;
        }
#endif

        static std::string key_of(const ParsedUrl &url) { return url.scheme + "://" + url.host_header; }

        std::shared_ptr<StreamCore> take(const std::string &key) {
            auto guard = pool.lock();
            auto it = guard->find(key);
            if (it == guard->end()) return nullptr;
            const auto now = std::chrono::steady_clock::now();
            while (!it->second.empty()) {
                Idle idle = std::move(it->second.back());
                it->second.pop_back();
                if (idle.core->open.load(std::memory_order_acquire) && now - idle.since <= options.idle_connection_timeout) {
                    return idle.core;
                }
                idle.core->request_close();
            }
            guard->erase(it);
            return nullptr;
        }

        void give_back(const std::string &key, std::shared_ptr<StreamCore> core) {
            auto guard = pool.lock();
            auto &bucket = (*guard)[key];
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(bucket, [&](Idle &idle) {
                const bool stale = !idle.core->open.load(std::memory_order_acquire) || now - idle.since > options.idle_connection_timeout;
                if (stale) idle.core->request_close();
                return stale;
            });
            while (bucket.size() >= options.max_idle_connections_per_host && !bucket.empty()) {
                bucket.front().core->request_close();
                bucket.pop_front();
            }
            if (options.max_idle_connections_per_host > 0) bucket.push_back({std::move(core), now});
            else core->request_close();
        }

        void close_all() {
            std::map<std::string, std::deque<Idle>> drained;
            {
                auto guard = pool.lock();
                drained.swap(*guard);
            }
            for (auto &entry : drained)
                for (auto &idle : entry.second) idle.core->request_close();
#if defined(STURDY_ASYNC_HTTP2)
        std::map<std::string, std::shared_ptr<H2Engine>> engines;
        {
            auto guard = h2_pool.lock();
            engines.swap(*guard);
        }
        for (auto &entry : engines) entry.second->close();
#endif
#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
        std::map<std::string, std::shared_ptr<H3Client>> h3_clients;
        {
            auto guard = h3_pool.lock();
            h3_clients.swap(*guard);
        }
        for (auto &entry : h3_clients) entry.second->close();
#endif
        }
    };

    namespace {

        bool idempotent(const std::string &method) {
            return method == "GET" || method == "HEAD" || method == "PUT" || method == "DELETE" || method == "OPTIONS";
        }

        IoError map_http_error(const ErrorCode &ec) {
            if (ec == http::error::body_limit || ec == http::error::header_limit) {
                return IoError{IoErrorCode::LimitExceeded, "HTTP response exceeds the configured limit"};
            }
            if (ec == http::error::end_of_stream) return IoError{IoErrorCode::UnexpectedEof, "connection closed before a response arrived"};
            if (ec.category() == http::make_error_code(http::error::bad_version).category()) {
                return IoError{IoErrorCode::ProtocolError, "malformed HTTP response: " + ec.message()};
            }
            return to_io_error("http", ec);
        }

        /// Resolves a Location header against the request URL. Empty result: unusable.
        std::string resolve_location(const ParsedUrl &base, std::string location) {
            if (const auto hash = location.find('#'); hash != std::string::npos) location.erase(hash);
            if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) return location;
            const std::string origin = base.scheme + "://" + base.host_header;
            if (location.rfind("//", 0) == 0) return base.scheme + ":" + location;
            if (!location.empty() && location.front() == '/') return origin + location;
            if (location.empty()) return {};
            std::string directory = base.target.substr(0, base.target.find('?'));
            directory.erase(directory.rfind('/') + 1);
            return origin + directory + location;
        }

        struct RequestOp : std::enable_shared_from_this<RequestOp> {
            std::shared_ptr<HttpClientCore> client;
            Strand strand{asio::make_strand(reactor())};
            asio::steady_timer deadline{strand};
            Promise<Result<HttpResponseMessage>> promise;
            CancellationRegistration cancel_registration;

            std::string method;
            ParsedUrl url;
            std::vector<HttpHeader> headers;
            std::string body;
            unsigned redirects_left = 0;

            std::shared_ptr<StreamCore> connection;
            std::optional<CoreStream> stream;
            std::string connection_key;
            bool reused = false;
            bool retried = false;
            bool h3_failed_for_this_request = false;
            bool finished = false;
            CancellationSource h2_cancel; ///< resets the current HTTP/2 stream if this request ends first
            beast::flat_buffer buffer;
            std::shared_ptr<http::request<http::string_body>> request;
            std::shared_ptr<http::response_parser<http::string_body>> parser;

            void start(const CancellationToken &token) {
                if (token.can_be_cancelled()) {
                    cancel_registration = token.on_cancel([weak = weak_from_this()] {
                        if (auto self = weak.lock()) asio::post(self->strand, [self] { self->cancel(); });
                    });
                }
                asio::post(strand, [self = shared_from_this()] {
                    self->deadline.expires_after(self->client->options.timeout);
                    self->deadline.async_wait(asio::bind_executor(self->strand, [self](const ErrorCode &ec) {
                        if (!ec) self->fail(IoError{IoErrorCode::TimedOut, "HTTP request timed out"});
                    }));
                    self->attempt();
                });
            }

            [[nodiscard]] bool via_proxy() const { return client->proxy.active && !bypasses_proxy(client->options.no_proxy, url.host); }

            void attempt() {
                if (finished) return;
                connection_key = HttpClientCore::key_of(url) + (via_proxy() ? "|via-proxy" : "");
#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
                if (url.secure && client->options.http3 && !via_proxy() && !h3_failed_for_this_request) {
                    if (auto h3 = client->h3_for(connection_key)) return send_h3(std::move(h3), false);
                    const std::optional<std::uint16_t> advertised = client->alt_service_port(connection_key);
                    if ((advertised || client->options.http3_prior_knowledge) && !client->h3_is_broken(connection_key)) {
                        return connect_h3_then_send(advertised.value_or(url.port));
                    }
                }
#endif
                attempt_tcp();
            }

            void attempt_tcp() {
#if defined(STURDY_ASYNC_HTTP2)
                if (url.secure && client->options.http2) {
                    if (auto engine = client->h2_for(connection_key)) return send_h2(std::move(engine), false);
                }
#endif
                if (client->options.keep_alive) {
                    if (auto pooled = client->take(connection_key)) {
                        adopt(std::move(pooled), true);
                        return;
                    }
                }
                connect_new();
            }

            void connect_new() {
                TaskHandle<Result<TcpConnection>> connecting;
                if (via_proxy()) {
                    connecting = url.secure ? open_tunnel(client->proxy, url, client->options.tls, client->options.timeout)
                                            : TcpConnection::connect(client->proxy.host, client->proxy.port, client->options.timeout);
                } else {
                    connecting = url.secure ? TcpConnection::connect_tls(url.host, url.port, client->options.tls, client->options.timeout)
                                            : TcpConnection::connect(url.host, url.port, client->options.timeout);
                }
                connecting.on_done([self = shared_from_this(), connecting]() mutable {
                    asio::post(self->strand, [self, connecting]() mutable {
                        if (self->finished) return;
                        try {
                            auto result = connecting.wait();
                            if (!result) return self->fail(std::move(result.error()));
                            self->adopt(StreamCore::take(std::move(*result)), false);
                        } catch (...) {
                            self->fail(IoError{IoErrorCode::Unknown, "connect failed"});
                        }
                    });
                });
            }

#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
            /// Opens (or waits for) an HTTP/3 connection to the origin, then sends. Any failure marks the origin as not worth
            /// trying over QUIC for a while and carries on with TCP, so a UDP-hostile network costs one short delay.
            void connect_h3_then_send(std::uint16_t port) {
                auto resolving = resolve_host(url.host, port);
                resolving.on_done([self = shared_from_this(), resolving, port]() mutable {
                    asio::post(self->strand, [self, resolving, port]() mutable {
                        if (self->finished) return;
                        try {
                            auto addresses = resolving.wait();
                            ErrorCode ec;
                            if (!addresses || addresses->empty()) return self->h3_fallback();
                            const udp::endpoint remote(asio::ip::make_address((*addresses)[0].host, ec), port);
                            if (ec) return self->h3_fallback();
                            H3ClientConfig config;
                            config.max_response_body_bytes = self->client->options.max_response_body_bytes;
                            config.max_header_bytes = self->client->options.max_response_header_bytes;
                            TlsClientOptions tls = self->client->options.tls;
                            tls.alpn.clear();
                            auto connecting = connect_h3(remote, self->url.host, std::move(tls), config, self->client->options.http3_connect_timeout);
                            connecting.on_done([self, connecting]() mutable {
                                asio::post(self->strand, [self, connecting]() mutable {
                                    if (self->finished) return;
                                    try {
                                        auto connected = connecting.wait();
                                        if (!connected) return self->h3_fallback();
                                        self->client->h3_register(self->connection_key, *connected);
                                        self->send_h3(*connected, true);
                                    } catch (...) {
                                        self->h3_fallback();
                                    }
                                });
                            });
                        } catch (...) {
                            self->h3_fallback();
                        }
                    });
                });
            }

            void h3_fallback() {
                client->mark_h3_broken(connection_key);
                h3_failed_for_this_request = true;
                attempt_tcp();
            }

            void send_h3(std::shared_ptr<H3Client> h3, bool fresh) {
                HttpRequestMessage message;
                message.method = method;
                message.target = url.target;
                message.body = body;
                message.headers.push_back({"user-agent", client->options.user_agent});
                message.headers.push_back({"accept", "*/*"});
                message.headers.push_back({"accept-encoding", client->options.decompress && kHaveZlib ? "gzip, deflate" : "identity"});
                if (client->jar && !header_value(headers, "cookie")) {
                    if (const std::string cookies = client->jar->header_for(url.host, url.target, url.secure); !cookies.empty()) {
                        message.headers.push_back({"cookie", cookies});
                    }
                }
                for (const HttpHeader &header : headers) message.headers.push_back(header);
                h2_cancel = CancellationSource();
                auto pending = h3->session->submit(std::move(message), url.scheme, url.host_header, h2_cancel.token());
                pending.on_done([self = shared_from_this(), pending, h3, fresh]() mutable {
                    asio::post(self->strand, [self, pending, h3, fresh]() mutable {
                        if (self->finished) return;
                        try {
                            auto result = pending.wait();
                            if (!result) {
                                // A pooled connection that died or said GOAWAY: try again on a fresh one (once, for safe methods).
                                if (!fresh && !self->retried && idempotent(self->method) && result.error().code == IoErrorCode::ConnectionReset) {
                                    self->retried = true;
                                    return self->attempt();
                                }
                                if (fresh && result.error().code != IoErrorCode::LimitExceeded) return self->h3_fallback(); // QUIC connected but the request failed: use TCP
                                return self->fail(std::move(result.error()));
                            }
                            HttpResponseMessage &response = *result;
                            self->process_response(response.status, std::move(response.headers), std::move(response.body), true, false);
                        } catch (...) {
                            self->fail(IoError{IoErrorCode::Unknown, "HTTP/3 request failed"});
                        }
                    });
                });
            }
#endif

#if defined(STURDY_ASYNC_HTTP2)
            /// Sends this request as a stream on a shared HTTP/2 connection. A connection that turned out to be dead
            /// (GOAWAY raced with us) is replaced once for methods that are safe to repeat.
            void send_h2(std::shared_ptr<H2Engine> engine, bool fresh) {
                HttpRequestMessage message;
                message.method = method;
                message.target = url.target;
                message.body = body;
                message.headers.push_back({"user-agent", client->options.user_agent});
                message.headers.push_back({"accept", "*/*"});
                message.headers.push_back({"accept-encoding", client->options.decompress && kHaveZlib ? "gzip, deflate" : "identity"});
                if (client->jar && !header_value(headers, "cookie")) {
                    if (const std::string cookies = client->jar->header_for(url.host, url.target, url.secure); !cookies.empty()) {
                        message.headers.push_back({"cookie", cookies});
                    }
                }
                for (const HttpHeader &header : headers) message.headers.push_back(header);
                h2_cancel = CancellationSource();
                auto pending = engine->submit(std::move(message), url.scheme, url.host_header, h2_cancel.token());
                pending.on_done([self = shared_from_this(), pending, fresh]() mutable {
                    asio::post(self->strand, [self, pending, fresh]() mutable {
                        if (self->finished) return;
                        try {
                            auto result = pending.wait();
                            if (!result) {
                                if (!fresh && !self->retried && idempotent(self->method) && result.error().code == IoErrorCode::ConnectionReset) {
                                    self->retried = true;
                                    return self->connect_new();
                                }
                                return self->fail(std::move(result.error()));
                            }
                            HttpResponseMessage &response = *result;
                            self->process_response(response.status, std::move(response.headers), std::move(response.body), true, false);
                        } catch (...) {
                            self->fail(IoError{IoErrorCode::Unknown, "HTTP/2 request failed"});
                        }
                    });
                });
            }
#endif

            void adopt(std::shared_ptr<StreamCore> core, bool from_pool) {
#if defined(STURDY_ASYNC_HTTP2)
                if (core->tls_details.alpn == "h2") {
                    auto engine = H2Engine::make_client(std::make_shared<CoreH2Transport>(core), H2ClientConfig{client->options.max_response_body_bytes, client->options.max_response_header_bytes}, [] {});
                    engine->start();
                    client->h2_register(connection_key, engine);
                    return send_h2(std::move(engine), true);
                }
#endif
                connection = std::move(core);
                reused = from_pool;
                stream.emplace(connection);
                buffer.consume(buffer.size());
                send_request();
            }

            void send_request() {
                const bool absolute_form = via_proxy() && !url.secure;
                request = std::make_shared<http::request<http::string_body>>(
                    http::string_to_verb(method), absolute_form ? url.scheme + "://" + url.host_header + url.target : url.target, 11);
                if (request->method() == http::verb::unknown) request->method_string(method);
                request->set(http::field::host, url.host_header);
                request->set(http::field::user_agent, client->options.user_agent);
                request->set(http::field::accept, "*/*");
                request->set(http::field::accept_encoding, client->options.decompress && kHaveZlib ? "gzip, deflate" : "identity");
                if (client->jar && !header_value(headers, "cookie")) {
                    if (const std::string cookies = client->jar->header_for(url.host, url.target, url.secure); !cookies.empty()) {
                        request->set(http::field::cookie, cookies);
                    }
                }
                if (absolute_form && !client->proxy.authorization.empty()) request->set(http::field::proxy_authorization, client->proxy.authorization);
                request->set(http::field::connection, client->options.keep_alive ? "keep-alive" : "close");
                for (const HttpHeader &header : headers) {
                    request->set(header.name, header.value);
                }
                if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
                    request->body() = body;
                    request->prepare_payload();
                }
                http::async_write(*stream, *request, asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec, std::size_t) {
                    if (self->finished) return;
                    if (ec) return self->transport_failed(ec, false);
                    self->read_response();
                }));
            }

            void read_response() {
                parser = std::make_shared<http::response_parser<http::string_body>>();
                parser->body_limit(client->options.max_response_body_bytes);
                parser->header_limit(client->options.max_response_header_bytes);
                if (method == "HEAD") parser->skip(true);
                http::async_read(*stream, buffer, *parser, asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec, std::size_t n) {
                    if (self->finished) return;
                    if (ec) return self->transport_failed(ec, n > 0);
                    self->got_response();
                }));
            }

            /// A pooled connection can have been closed by the server while idle: that shows up as a failure with
            /// nothing received. Retry once on a fresh connection, but only for methods that are safe to repeat.
            void transport_failed(const ErrorCode &ec, bool received_bytes) {
                connection->request_close();
                if (reused && !retried && !received_bytes && idempotent(method)) {
                    retried = true;
                    reused = false;
                    return connect_new();
                }
                fail(map_http_error(ec));
            }

            void got_response() {
                auto response = parser->release();
                std::vector<HttpHeader> raw;
                for (const auto &field : response) raw.push_back({std::string(field.name_string()), std::string(field.value())});
                const bool reusable = client->options.keep_alive && response.keep_alive() && buffer.size() == 0;
                process_response(response.result_int(), std::move(raw), std::move(response.body()), response.keep_alive(), reusable);
            }

            /// Cookies, redirects and decoding: shared by HTTP/1.1 and HTTP/2.
            void process_response(unsigned status, std::vector<HttpHeader> response_headers, std::string response_body, bool keep_alive, bool reusable) {
                if (client->jar) client->jar->store_from_response(url.host, url.target, url.secure, response_headers);
#if defined(STURDY_ASYNC_HTTP3) && defined(STURDY_ASYNC_QUIC)
                if (client->options.http3 && url.secure && !via_proxy()) {
                    if (auto alt_svc = header_value(response_headers, "alt-svc")) client->learn_alt_svc(connection_key, url.host, *alt_svc);
                }
#endif

                const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
                const auto location = header_value(response_headers, "location");
                if (redirect && redirects_left > 0 && location) {
                    const std::string target = resolve_location(url, std::string(*location));
                    auto next = target.empty() ? std::unexpected(IoError{IoErrorCode::ProtocolError, "unusable Location header"})
                                               : parse_url(target, {"http", "https"});
                    if (!next) return fail(std::move(next.error()));
                    if (url.secure && !next->secure && !client->options.allow_https_downgrade) {
                        return fail(IoError{IoErrorCode::PermissionDenied, "refusing to follow a redirect from https to http"});
                    }
                    release_connection(reusable);
                    if (next->scheme != url.scheme || next->host != url.host || next->port != url.port) {
                        std::erase_if(headers, [](const HttpHeader &h) {
                            return header_value({h}, "authorization") || header_value({h}, "cookie") || header_value({h}, "proxy-authorization");
                        });
                    }
                    if (status == 303 || ((status == 301 || status == 302) && method == "POST")) {
                        if (method != "HEAD") method = "GET";
                        body.clear();
                        std::erase_if(headers, [](const HttpHeader &h) {
                            return header_value({h}, "content-type") || header_value({h}, "content-length");
                        });
                    }
                    url = std::move(*next);
                    --redirects_left;
                    retried = false;
                    h3_failed_for_this_request = false;
                    return attempt();
                }

                HttpResponseMessage result;
                result.status = status;
                result.keep_alive = keep_alive;
                result.body = std::move(response_body);
                result.headers = std::move(response_headers);
                result.final_url = url.scheme + "://" + url.host_header + url.target;
                if (client->options.decompress && kHaveZlib && method != "HEAD" && status != 204 && status != 304 && !result.body.empty()) {
                    if (auto encoding = header_value(result.headers, "content-encoding")) {
                        std::string lowered(*encoding);
                        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (lowered == "gzip" || lowered == "x-gzip" || lowered == "deflate") {
                            auto decoded = inflate_body(result.body, client->options.max_response_body_bytes);
                            if (!decoded) {
                                release_connection(false);
                                return fail(std::move(decoded.error()));
                            }
                            result.body = std::move(*decoded);
                            std::erase_if(result.headers, [](const HttpHeader &h) { return header_value({h}, "content-encoding") || header_value({h}, "content-length"); });
                            result.headers.push_back({"Content-Length", std::to_string(result.body.size())});
                        }
                    }
                }
                release_connection(reusable);
                succeed(std::move(result));
            }

            void release_connection(bool reusable) {
                if (connection && reusable && connection->open.load(std::memory_order_acquire)) {
                    client->give_back(connection_key, connection);
                } else if (connection) {
                    connection->request_close();
                }
                stream.reset();
                connection.reset();
            }

            void settle() {
                finished = true;
                (void)h2_cancel.cancel();
                cancel_registration.reset();
                deadline.cancel();
            }

            void succeed(HttpResponseMessage response) {
                if (finished) return;
                settle();
                (void)promise.set_value(Result<HttpResponseMessage>(std::move(response)));
            }

            void fail(IoError error) {
                if (finished) return;
                settle();
                if (connection) connection->request_close();
                (void)promise.set_value(Result<HttpResponseMessage>(std::unexpected(std::move(error))));
            }

            void cancel() {
                if (finished) return;
                settle();
                if (connection) connection->request_close();
                (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            }
        };

    } // namespace
} // namespace SFT::Async::Detail

namespace SFT::Async {
    using namespace Detail;

    HttpClient::HttpClient(HttpClientOptions options) : core_(std::make_shared<HttpClientCore>(std::move(options))) {}
    HttpClient::~HttpClient() {
        if (core_) core_->close_all();
    }
    HttpClient::HttpClient(HttpClient &&) noexcept = default;
    HttpClient &HttpClient::operator=(HttpClient &&other) noexcept {
        if (this != &other) {
            if (core_) core_->close_all();
            core_ = std::move(other.core_);
        }
        return *this;
    }

    std::shared_ptr<CookieJar> HttpClient::cookie_jar() const { return core_ ? core_->jar : nullptr; }

    void HttpClient::close_idle() noexcept {
        if (core_) core_->close_all();
    }

    TaskHandle<std::expected<HttpResponseMessage, IoError>> HttpClient::request(std::string method, std::string url, std::vector<HttpHeader> headers,
                                                                                std::string body, CancellationToken cancellation) {
        if (!core_) return failed<HttpResponseMessage>(closed_error("HTTP client moved from"));
        if (method.empty() || has_line_break(method) || method.find(' ') != std::string::npos) {
            return failed<HttpResponseMessage>(IoError{IoErrorCode::InvalidArgument, "invalid HTTP method"});
        }
        for (const auto &header : headers) {
            if (header.name.empty() || has_line_break(header.name) || has_line_break(header.value) || header.name.find(':') != std::string::npos) {
                return failed<HttpResponseMessage>(IoError{IoErrorCode::InvalidArgument, "request headers must not contain line breaks"});
            }
        }
        auto parsed = parse_url(url, {"http", "https"});
        if (!parsed) return failed<HttpResponseMessage>(parsed.error());
        if (cancellation.is_cancelled()) return cancelled_handle<HttpResponseMessage>();

        // Headers the client owns; callers cannot set or smuggle them.
        std::erase_if(headers, [](const HttpHeader &h) {
            return header_value({h}, "host") || header_value({h}, "content-length") || header_value({h}, "connection") ||
                   header_value({h}, "transfer-encoding");
        });
        auto op = std::make_shared<RequestOp>();
        op->client = core_;
        op->method = std::move(method);
        op->url = std::move(*parsed);
        op->headers = std::move(headers);
        op->body = std::move(body);
        op->redirects_left = core_->options.max_redirects;
        auto handle = op->promise.handle();
        op->start(cancellation);
        return handle;
    }

} // namespace SFT::Async

#else

namespace SFT::Async {
    HttpClient::HttpClient(HttpClientOptions) {}
    HttpClient::~HttpClient() = default;
    HttpClient::HttpClient(HttpClient &&) noexcept = default;
    HttpClient &HttpClient::operator=(HttpClient &&) noexcept = default;
    std::shared_ptr<CookieJar> HttpClient::cookie_jar() const { return nullptr; }
    void HttpClient::close_idle() noexcept {}
    TaskHandle<std::expected<HttpResponseMessage, IoError>> HttpClient::request(std::string, std::string, std::vector<HttpHeader>, std::string, CancellationToken) {
        Promise<std::expected<HttpResponseMessage, IoError>> promise;
        (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "HttpClient is not implemented on Web yet (use fetch)"}));
        return promise.handle();
    }
} // namespace SFT::Async

#endif
