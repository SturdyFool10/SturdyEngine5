#include <Async/BindingInternal.hpp>

#include <Async/Channel.hpp>
#include <Async/HttpClient.hpp>
#include <Async/HttpServer.hpp>
#include <Async/HttpStreaming.hpp>
#include <Async/RateLimiter.hpp>
#include <Async/Router.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

namespace SFT::Async::Binding {

    using namespace Detail;

    namespace {
        constexpr const char *kBody = "http_body";
        constexpr const char *kLimiter = "rate_limiter";

        // ---------------------------------------------------------------------------------------------------------
        // Streamed response bodies
        // ---------------------------------------------------------------------------------------------------------
        /// State shared between the writer object and the server's pull function. It never holds the channel's Sender, so
        /// releasing the writer closes the channel (and so ends the body) even while the server still holds this.
        struct BodyShared {
            struct Waiter {
                OpPtr op;
                CancellationRegistration cancel;
            };
            CancellationSource closed;
            std::mutex mutex;
            bool is_closed = false;
            std::vector<Waiter> waiters;

            void mark_closed() {
                std::vector<Waiter> ready;
                {
                    std::lock_guard lock(mutex);
                    if (is_closed) return;
                    is_closed = true;
                    ready.swap(waiters);
                }
                (void)closed.cancel();
                for (auto &w : ready) w.op->complete(Value{});
            }
        };

        struct BodyWriter {
            Sender<std::string> sender;
            std::shared_ptr<BodyShared> shared;
        };

        /// Wires `message.body_stream` to a fresh bounded channel and returns the writer object for the foreign side.
        ObjectPtr attach_stream(HttpResponseMessage &message, std::size_t queue) {
            auto pair = channel<std::string>((std::max<std::size_t>)(queue, 1));
            auto shared = std::make_shared<BodyShared>();
            auto armed = std::make_shared<std::atomic<bool>>(false);
            auto registration = std::make_shared<CancellationRegistration>();
            message.body_stream = [receiver = pair.receiver, shared, armed, registration](CancellationToken cancel) mutable {
                if (!armed->exchange(true) && cancel.can_be_cancelled()) {
                    // The server cancels this token when the client goes away or the server stops: wake blocked writers.
                    *registration = cancel.on_cancel([receiver, weak = std::weak_ptr<BodyShared>(shared)]() mutable {
                        receiver.close();
                        if (auto s = weak.lock()) s->mark_closed();
                    });
                }
                return receiver.recv(std::move(cancel));
            };
            return make_object<std::shared_ptr<BodyWriter>>(kBody, std::make_shared<BodyWriter>(BodyWriter{std::move(pair.sender), std::move(shared)}));
        }

        bool has_header(const Fields &fields, std::string_view name) {
            for (const auto &[key, value] : fields) {
                if (key.size() == name.size() && std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) return true;
            }
            return false;
        }

        /// Turns a response description into the server's message (and the body writer, for streamed responses).
        HttpResponseMessage build_message(HttpResponseSpec &&r, ObjectPtr *writer) {
            const bool streamed = r.stream || r.sse;
            const int sources = (!r.body.empty() ? 1 : 0) + (!r.body_file.empty() ? 1 : 0) + (streamed ? 1 : 0);
            if (sources > 1) throw BindingError(IoErrorCode::InvalidArgument, "a response has one body: bytes, a file, or a stream");
            if (r.sse) {
                if (!has_header(r.headers, "Content-Type")) r.headers.emplace_back("Content-Type", "text/event-stream");
                if (!has_header(r.headers, "Cache-Control")) r.headers.emplace_back("Cache-Control", "no-cache");
                if (!has_header(r.headers, "X-Accel-Buffering")) r.headers.emplace_back("X-Accel-Buffering", "no");
            }
            HttpResponseMessage message;
            message.status = r.status;
            message.headers = to_headers(r.headers);
            message.keep_alive = r.keep_alive;
            if (!r.body_file.empty()) {
                message.body_file = std::move(r.body_file);
                message.body_offset = r.body_offset;
                if (r.body_length >= 0) message.body_length = static_cast<std::uint64_t>(r.body_length);
            } else if (streamed) {
                ObjectPtr object = attach_stream(message, r.stream_queue);
                if (writer) *writer = std::move(object);
            } else {
                message.body.assign(r.body.begin(), r.body.end());
            }
            return message;
        }

        HttpResponseSpec to_spec(const HttpResponseMessage &m) {
            HttpResponseSpec s;
            s.status = m.status;
            s.headers = from_headers(m.headers);
            s.keep_alive = m.keep_alive;
            s.body.assign(m.body.begin(), m.body.end());
            s.body_file = m.body_file;
            s.body_offset = m.body_offset;
            s.body_length = m.body_length ? static_cast<std::int64_t>(*m.body_length) : -1;
            s.stream = static_cast<bool>(m.body_stream);
            return s;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Requests and handlers
        // ---------------------------------------------------------------------------------------------------------
        HttpRequestView view_of(HttpRequestMessage &&request) {
            HttpRequestView view;
            view.method = std::move(request.method);
            view.target = std::move(request.target);
            view.remote_address = std::move(request.remote_address);
            view.version = request.version;
            view.headers = from_headers(request.headers);
            view.body.assign(request.body.begin(), request.body.end());
            view.body_file = std::move(request.body_file);
            return view;
        }
        HttpRequestView view_of(const RouteRequest &request) {
            HttpRequestView view = view_of(HttpRequestMessage(request.http));
            view.path = request.path;
            view.query = request.query;
            for (const auto &[name, value] : request.params) view.params.emplace_back(name, value);
            return view;
        }
        HttpRequestMessage message_of(HttpRequestView &&view) {
            HttpRequestMessage m;
            m.method = std::move(view.method);
            m.target = std::move(view.target);
            m.version = view.version;
            m.headers = to_headers(view.headers);
            m.body.assign(view.body.begin(), view.body.end());
            m.remote_address = std::move(view.remote_address);
            m.body_file = std::move(view.body_file);
            return m;
        }

        /// Runs `handler` for one request and gives back the handle its `respond` call completes.
        template <typename View>
        TaskHandle<HttpResponseMessage> run_handler(const HttpHandler &handler, View &&view) {
            auto promise = std::make_shared<Promise<HttpResponseMessage>>();
            auto result = promise->handle();
            auto answered = std::make_shared<std::atomic<bool>>(false);
            HttpRespond respond = [promise, answered](HttpResponseSpec response) -> ObjectPtr {
                ObjectPtr writer;
                HttpResponseMessage message = build_message(std::move(response), &writer); // validates before the one-shot is spent
                if (answered->exchange(true)) return {};
                (void)promise->set_value(std::move(message));
                return writer;
            };
            try {
                handler(std::forward<View>(view), std::move(respond));
            } catch (const std::exception &error) {
                if (!answered->exchange(true)) {
                    (void)promise->set_value(text_response(500, std::string("handler failed: ") + error.what()));
                }
            } catch (...) {
                if (!answered->exchange(true)) (void)promise->set_value(text_response(500, "handler failed"));
            }
            return result;
        }

        HttpServerOptions to_options(const HttpServerSpec &spec) {
            HttpServerOptions o;
            o.bind_address = spec.bind_address;
            o.port = spec.port;
            o.tls_certificate_chain_file = spec.tls_certificate_chain_file;
            o.tls_private_key_file = spec.tls_private_key_file;
            o.io_threads = spec.io_threads;
            o.handler_threads = spec.handler_threads;
            o.max_connections = spec.max_connections;
            o.max_request_body_bytes = spec.max_request_body_bytes;
            o.max_request_header_bytes = spec.max_request_header_bytes;
            o.max_response_body_bytes = spec.max_response_body_bytes;
            o.shutdown_timeout_ms = spec.shutdown_timeout_ms;
            o.max_connections_per_ip = spec.max_connections_per_ip;
            o.spool_requests_above = spec.spool_requests_above;
            o.max_spooled_body_bytes = spec.max_spooled_body_bytes;
            o.enable_http2 = spec.enable_http2;
            o.http2_max_concurrent_streams = spec.http2_max_concurrent_streams;
            o.enable_http3 = spec.enable_http3;
            o.http3_port = spec.http3_port;
            o.http3_max_concurrent_streams = spec.http3_max_concurrent_streams;
            o.http3_max_connections = spec.http3_max_connections;
            o.http3_max_connections_per_address = spec.http3_max_connections_per_address;
            o.websocket.max_message_bytes = spec.websocket_max_message_bytes;
            o.websocket.max_queued_send_bytes = spec.websocket_max_queued_send_bytes;
            o.websocket.idle_timeout = ms(spec.websocket_idle_timeout_ms);
            o.websocket.compression = spec.websocket_compression;
            o.websocket_allowed_origins = spec.websocket_allowed_origins;
            o.websocket_subprotocols = spec.websocket_subprotocols;
            o.max_websockets = spec.max_websockets;
            return o;
        }

        Router &router_of(const ObjectPtr &router) { return get<Router>(router, kRouter); }
    } // namespace

    // =================================================================================================================
    // HTTP server
    // =================================================================================================================
    ObjectPtr http_server_create(const HttpServerSpec &spec, HttpHandler handler) {
        if (!handler) throw BindingError(IoErrorCode::InvalidArgument, "an HTTP handler is required");
        HttpServer server(to_options(spec), HttpServer::AsyncHandler([handler = std::move(handler)](HttpRequestMessage request) {
                              return run_handler(handler, view_of(std::move(request)));
                          }));
        return make_object<HttpServer>(kHttpServer, std::move(server));
    }
    ObjectPtr http_server_create_router(const HttpServerSpec &spec, const ObjectPtr &router) {
        HttpServer server(to_options(spec), router_of(router).handler());
        return make_object<HttpServer>(kHttpServer, std::move(server));
    }
    void http_server_on_websocket(const ObjectPtr &server, const std::string &path, WebSocketHandler handler) {
        if (!handler) throw BindingError(IoErrorCode::InvalidArgument, "a WebSocket handler is required");
        get<HttpServer>(server, kHttpServer).on_websocket(path, [handler = std::move(handler)](WebSocket socket, HttpRequestMessage request) {
            handler(make_object<WebSocket>(kWebSocket, std::move(socket)), view_of(std::move(request)));
        });
    }
    void http_server_begin(const ObjectPtr &server) {
        if (auto started = get<HttpServer>(server, kHttpServer).start(); !started) throw BindingError(started.error().code, started.error().message);
    }
    ObjectPtr http_server_start(const HttpServerSpec &spec, HttpHandler handler) {
        auto server = http_server_create(spec, std::move(handler));
        http_server_begin(server);
        return server;
    }
    ObjectPtr http_server_start_router(const HttpServerSpec &spec, const ObjectPtr &router) {
        auto server = http_server_create_router(spec, router);
        http_server_begin(server);
        return server;
    }
    std::uint16_t http_server_port(const ObjectPtr &server) {
        auto port = get<HttpServer>(server, kHttpServer).local_port();
        if (!port) throw BindingError(port.error().code, port.error().message);
        return *port;
    }
    std::uint16_t http_server_http3_port(const ObjectPtr &server) {
        auto port = get<HttpServer>(server, kHttpServer).http3_local_port();
        return port ? *port : 0;
    }
    Fields http_server_info(const ObjectPtr &server) {
        auto &s = get<HttpServer>(server, kHttpServer);
        auto flag = [](bool v) { return std::string(v ? "1" : "0"); };
        Fields fields{{"running", flag(s.is_running())}, {"tls", flag(s.uses_tls())}, {"http2", flag(s.offers_http2())}};
        if (auto port = s.local_port()) fields.emplace_back("port", std::to_string(*port));
        if (auto port = s.http3_local_port()) fields.emplace_back("http3_port", std::to_string(*port));
        return fields;
    }
    Fields http_server_metrics(const ObjectPtr &server) {
        const auto m = get<HttpServer>(server, kHttpServer).metrics();
        auto n = [](std::uint64_t v) { return std::to_string(v); };
        return {{"connections_accepted", n(m.connections_accepted)},
                {"connections_rejected_limit", n(m.connections_rejected_limit)},
                {"connections_rejected_per_ip", n(m.connections_rejected_per_ip)},
                {"requests", n(m.requests)},
                {"responses_2xx", n(m.responses_2xx)},
                {"responses_3xx", n(m.responses_3xx)},
                {"responses_4xx", n(m.responses_4xx)},
                {"responses_5xx", n(m.responses_5xx)},
                {"websocket_upgrades", n(m.websocket_upgrades)},
                {"active_connections", n(m.active_connections)},
                {"active_websockets", n(m.active_websockets)}};
    }
    void http_server_stop(const ObjectPtr &server) { get<HttpServer>(server, kHttpServer).stop(); }

    // =================================================================================================================
    // Streamed bodies
    // =================================================================================================================
    OpPtr http_body_write(const ObjectPtr &body, Bytes data) {
        auto writer = get<std::shared_ptr<BodyWriter>>(body, kBody);
        return launch([writer, data = std::move(data)](CancellationToken) mutable { return writer->sender.send(to_string_bytes(data)); }, [](Value &) {});
    }
    std::string format_sse(const SseEventSpec &event) {
        SseEvent e;
        e.data = event.data;
        e.event = event.event;
        e.id = event.id;
        if (event.retry_ms >= 0) e.retry_ms = static_cast<unsigned>(event.retry_ms);
        return SFT::Async::format_sse(e);
    }
    OpPtr http_body_write_sse(const ObjectPtr &body, const SseEventSpec &event) {
        auto writer = get<std::shared_ptr<BodyWriter>>(body, kBody);
        return launch([writer, text = format_sse(event)](CancellationToken) mutable { return writer->sender.send(std::move(text)); }, [](Value &) {});
    }
    void http_body_finish(const ObjectPtr &body) { get<std::shared_ptr<BodyWriter>>(body, kBody)->sender.close(); }
    OpPtr http_body_closed(const ObjectPtr &body) {
        auto writer = get<std::shared_ptr<BodyWriter>>(body, kBody);
        auto op = std::make_shared<Op>();
        BodyShared &shared = *writer->shared;
        std::lock_guard lock(shared.mutex);
        if (shared.is_closed) {
            op->complete(Value{});
            return op;
        }
        CancellationRegistration registration = op->token().on_cancel([weak = std::weak_ptr<Op>(op)] {
            if (auto o = weak.lock()) o->mark_cancelled();
        });
        shared.waiters.push_back({op, std::move(registration)});
        return op;
    }

    std::string percent_decode(const std::string &text, bool plus_is_space) {
        auto decoded = SFT::Async::percent_decode(text, plus_is_space);
        if (!decoded) throw BindingError(IoErrorCode::InvalidArgument, "malformed percent-encoding");
        return *decoded;
    }
    HttpResponseSpec canned_response(ResponseKind kind, std::uint32_t status, const std::string &body) {
        HttpResponseMessage m;
        switch (kind) {
            case ResponseKind::Text: m = text_response(status, body); break;
            case ResponseKind::Html: m = html_response(status, body); break;
            case ResponseKind::Json: m = json_response(status, body); break;
            case ResponseKind::Redirect: m = redirect_response(body, status); break;
        }
        return to_spec(m);
    }

    // =================================================================================================================
    // Router
    // =================================================================================================================
    ObjectPtr router_new() { return make_object<Router>(kRouter, Router{}); }
    void router_route(const ObjectPtr &router, const std::string &method, const std::string &pattern, HttpHandler handler) {
        if (!handler) throw BindingError(IoErrorCode::InvalidArgument, "a route handler is required");
        router_of(router).route_async(method, pattern, [handler = std::move(handler)](const RouteRequest &request) { return run_handler(handler, view_of(request)); });
    }
    void router_use(const ObjectPtr &router, HttpMiddleware middleware) {
        if (!middleware) throw BindingError(IoErrorCode::InvalidArgument, "a middleware is required");
        router_of(router).use([middleware = std::move(middleware)](RouteRequest &request) -> std::optional<HttpResponseMessage> {
            struct Slot {
                std::mutex mutex;
                std::optional<HttpResponseMessage> message;
                bool closed = false;
            };
            auto slot = std::make_shared<Slot>();
            HttpRespond respond = [slot](HttpResponseSpec response) -> ObjectPtr {
                ObjectPtr writer;
                HttpResponseMessage message = build_message(std::move(response), &writer);
                std::lock_guard lock(slot->mutex);
                if (slot->closed || slot->message) return {};
                slot->message = std::move(message);
                return writer;
            };
            try {
                middleware(view_of(request), std::move(respond));
            } catch (const std::exception &error) {
                return text_response(500, std::string("middleware failed: ") + error.what());
            }
            std::lock_guard lock(slot->mutex);
            slot->closed = true;
            return std::move(slot->message);
        });
    }
    void router_static_files(const ObjectPtr &router, const std::string &mount, const StaticFilesSpec &spec) {
        StaticFilesOptions o;
        o.root = spec.root;
        o.index_file = spec.index_file;
        o.cache_control = spec.cache_control;
        o.allow_dotfiles = spec.allow_dotfiles;
        o.follow_symlinks = spec.follow_symlinks;
        o.serve_precompressed = spec.serve_precompressed;
        o.spa_fallback = spec.spa_fallback;
        for (const auto &[extension, type] : spec.mime_types) o.mime_types[extension] = type;
        try {
            router_of(router).static_files(mount, std::move(o));
        } catch (const std::invalid_argument &error) {
            throw BindingError(IoErrorCode::InvalidArgument, error.what());
        }
    }
    void router_not_found(const ObjectPtr &router, HttpHandler handler) {
        if (!handler) throw BindingError(IoErrorCode::InvalidArgument, "a handler is required");
        router_of(router).not_found([handler = std::move(handler)](const RouteRequest &request) {
            // `not_found` is synchronous: wait for the handler's answer (it normally answers inline).
            auto handle = run_handler(handler, view_of(request));
            return handle.wait();
        });
    }
    void router_rate_limit(const ObjectPtr &router, double rate_per_second, double burst, std::uint64_t max_keys) {
        router_of(router).use(rate_limit_middleware(std::make_shared<RateLimiter>(rate_per_second, burst, static_cast<std::size_t>(max_keys))));
    }
    HttpResponseSpec router_dispatch(const ObjectPtr &router, HttpRequestView request) {
        return to_spec(router_of(router).dispatch(message_of(std::move(request))));
    }

    // =================================================================================================================
    // Rate limiter, cookie jar
    // =================================================================================================================
    ObjectPtr rate_limiter_new(double rate_per_second, double burst, std::uint64_t max_keys) {
        return make_object<std::shared_ptr<RateLimiter>>(kLimiter, std::make_shared<RateLimiter>(rate_per_second, burst, static_cast<std::size_t>(max_keys)));
    }
    bool rate_limiter_try_acquire(const ObjectPtr &limiter, const std::string &key, double cost) {
        return get<std::shared_ptr<RateLimiter>>(limiter, kLimiter)->try_acquire(key, cost);
    }
    std::int64_t rate_limiter_retry_after_ms(const ObjectPtr &limiter, const std::string &key, double cost) {
        return get<std::shared_ptr<RateLimiter>>(limiter, kLimiter)->retry_after(key, cost).count();
    }
    std::uint64_t rate_limiter_tracked_keys(const ObjectPtr &limiter) { return get<std::shared_ptr<RateLimiter>>(limiter, kLimiter)->tracked_keys(); }

    ObjectPtr cookie_jar_new() { return make_object<std::shared_ptr<CookieJar>>(kCookieJar, std::make_shared<CookieJar>()); }
    void cookie_jar_add(const ObjectPtr &jar, const CookieSpec &spec) {
        Cookie c;
        c.name = spec.name;
        c.value = spec.value;
        c.domain = spec.domain;
        c.path = spec.path.empty() ? "/" : spec.path;
        c.host_only = spec.host_only;
        c.secure = spec.secure;
        c.http_only = spec.http_only;
        if (spec.expires_unix >= 0) c.expires = std::chrono::system_clock::time_point(std::chrono::seconds(spec.expires_unix));
        get<std::shared_ptr<CookieJar>>(jar, kCookieJar)->add(std::move(c));
    }
    void cookie_jar_store_from_response(const ObjectPtr &jar, const std::string &host, const std::string &path, bool https, const Fields &response_headers) {
        get<std::shared_ptr<CookieJar>>(jar, kCookieJar)->store_from_response(host, path, https, to_headers(response_headers));
    }
    std::string cookie_jar_header_for(const ObjectPtr &jar, const std::string &host, const std::string &path, bool https) {
        return get<std::shared_ptr<CookieJar>>(jar, kCookieJar)->header_for(host, path, https);
    }
    std::vector<CookieSpec> cookie_jar_all(const ObjectPtr &jar) {
        std::vector<CookieSpec> out;
        for (const Cookie &c : get<std::shared_ptr<CookieJar>>(jar, kCookieJar)->all()) {
            CookieSpec s;
            s.name = c.name;
            s.value = c.value;
            s.domain = c.domain;
            s.path = c.path;
            s.host_only = c.host_only;
            s.secure = c.secure;
            s.http_only = c.http_only;
            if (c.expires) s.expires_unix = std::chrono::duration_cast<std::chrono::seconds>(c.expires->time_since_epoch()).count();
            out.push_back(std::move(s));
        }
        return out;
    }
    void cookie_jar_clear(const ObjectPtr &jar) { get<std::shared_ptr<CookieJar>>(jar, kCookieJar)->clear(); }

} // namespace SFT::Async::Binding
