/// C ABI for HTTP serving: servers, routers, streamed bodies, WebSocket upgrades, the rate limiter and the cookie jar.
/// A thin translation of the serving half of `Async/Binding.hpp`; see AsyncNetSupport.hpp for the shared helpers.

#include <FFI/AsyncNetSupport.hpp>

using namespace SFT::Ffi::AsyncSupport;

namespace {

    /// Hands a request to a foreign callback as the C struct (valid for the duration of `fn`).
    template <typename Fn>
    void with_request(const B::HttpRequestView &request, Fn &&fn) {
        std::vector<SturdyKeyValue> headers;
        headers.reserve(request.headers.size());
        for (const auto &[name, value] : request.headers) headers.push_back({name.c_str(), value.c_str()});
        std::vector<SturdyKeyValue> params;
        params.reserve(request.params.size());
        for (const auto &[name, value] : request.params) params.push_back({name.c_str(), value.c_str()});
        SturdyHttpRequest view{};
        view.method = request.method.c_str();
        view.target = request.target.c_str();
        view.remote_address = request.remote_address.c_str();
        view.version = request.version;
        view.headers = headers.data();
        view.header_count = static_cast<uint32_t>(headers.size());
        view.body = request.body.data();
        view.body_size = request.body.size();
        view.body_file = request.body_file.empty() ? nullptr : request.body_file.c_str();
        view.path = request.path.empty() ? nullptr : request.path.c_str();
        view.query = request.path.empty() ? nullptr : request.query.c_str();
        view.params = params.data();
        view.param_count = static_cast<uint32_t>(params.size());
        fn(view);
    }

    /// Wraps a C handler as a facade handler: each request gets a responder handle that `sturdy_http_respond*` consumes.
    /// `middleware`: the responder only counts if it is used before the callback returns, so an unused one is discarded afterwards.
    B::HttpHandler adapt(SturdyHttpHandlerFn handler, void *user_data, bool middleware = false) {
        return [handler, user_data, middleware](B::HttpRequestView request, B::HttpRespond respond) {
            auto pending = std::make_unique<B::HttpRespond>(std::move(respond));
            void *pointer = pending.get();
            const u64 token = mint_handle(HandleKind::HttpResponder, pointer);
            {
                const std::lock_guard<std::mutex> lock{g_mutex};
                g_responders.emplace(token, std::move(pending));
            }
            no_throw([&] { with_request(request, [&](const SturdyHttpRequest &view) { handler(user_data, &view, SturdyHttpResponder{token}); }); });
            if (middleware) {
                std::unique_ptr<B::HttpRespond> unused;
                {
                    const std::lock_guard<std::mutex> lock{g_mutex};
                    if (auto it = g_responders.find(token); it != g_responders.end()) {
                        unused = std::move(it->second);
                        g_responders.erase(it);
                    }
                }
                if (unused) revoke_handle(token);
            }
        };
    }

    B::HttpServerSpec server_spec_of(const SturdyHttpServerOptions *options) {
        B::HttpServerSpec spec;
        if (options == nullptr) return spec;
        if (options->bind_address != nullptr) spec.bind_address = options->bind_address;
        spec.port = options->port;
        spec.tls_certificate_chain_file = text(options->tls_certificate_chain_file);
        spec.tls_private_key_file = text(options->tls_private_key_file);
        spec.io_threads = options->io_threads;
        spec.handler_threads = options->handler_threads;
        if (options->max_connections != 0) spec.max_connections = options->max_connections;
        if (options->max_request_body_bytes != 0) spec.max_request_body_bytes = options->max_request_body_bytes;
        if (options->max_request_header_bytes != 0) spec.max_request_header_bytes = options->max_request_header_bytes;
        if (options->max_response_body_bytes != 0) spec.max_response_body_bytes = options->max_response_body_bytes;
        if (options->shutdown_timeout_ms != 0) spec.shutdown_timeout_ms = options->shutdown_timeout_ms;
        spec.max_connections_per_ip = options->max_connections_per_ip;
        spec.spool_requests_above = options->spool_requests_above;
        if (options->max_spooled_body_bytes != 0) spec.max_spooled_body_bytes = options->max_spooled_body_bytes;
        spec.enable_http2 = options->no_http2 == STURDY_FALSE;
        if (options->http2_max_concurrent_streams != 0) spec.http2_max_concurrent_streams = options->http2_max_concurrent_streams;
        spec.enable_http3 = options->enable_http3 != STURDY_FALSE;
        spec.http3_port = options->http3_port;
        if (options->http3_max_concurrent_streams != 0) spec.http3_max_concurrent_streams = options->http3_max_concurrent_streams;
        if (options->http3_max_connections != 0) spec.http3_max_connections = options->http3_max_connections;
        if (options->http3_max_connections_per_address != 0) spec.http3_max_connections_per_address = options->http3_max_connections_per_address;
        if (options->websocket_max_message_bytes != 0) spec.websocket_max_message_bytes = options->websocket_max_message_bytes;
        if (options->websocket_max_queued_send_bytes != 0) spec.websocket_max_queued_send_bytes = options->websocket_max_queued_send_bytes;
        if (options->websocket_idle_timeout_ms != 0) spec.websocket_idle_timeout_ms = options->websocket_idle_timeout_ms;
        spec.websocket_compression = options->websocket_compression != STURDY_FALSE;
        spec.websocket_allowed_origins = strings(options->websocket_allowed_origins, options->websocket_allowed_origin_count);
        spec.websocket_subprotocols = strings(options->websocket_subprotocols, options->websocket_subprotocol_count);
        if (options->max_websockets != 0) spec.max_websockets = options->max_websockets;
        return spec;
    }

    B::SseEventSpec sse_of(const SturdySseEvent &e) {
        B::SseEventSpec spec;
        spec.data = text(e.data);
        spec.event = text(e.event);
        spec.id = text(e.id);
        spec.retry_ms = e.has_retry != STURDY_FALSE ? static_cast<std::int64_t>(e.retry_ms) : -1;
        return spec;
    }

    /// Sends `response` through the responder; the responder is consumed only when the facade accepted the response.
    SturdyResult send_response(SturdyHttpResponder responder, B::HttpResponseSpec response, SturdyAsyncObject *out_body) {
        void *pointer = nullptr;
        if (const auto r = resolve_handle(responder.token, HandleKind::HttpResponder, &pointer); r != STURDY_OK) return r;
        B::HttpRespond function;
        {
            const std::lock_guard<std::mutex> lock{g_mutex};
            auto it = g_responders.find(responder.token);
            if (it == g_responders.end()) return set_error(STURDY_ERROR_HANDLE_EXPIRED, "the request was already answered");
            function = *it->second;
        }
        B::ObjectPtr writer = function(std::move(response)); // throws BindingError for an invalid response, leaving the responder usable
        std::unique_ptr<B::HttpRespond> consumed;
        {
            const std::lock_guard<std::mutex> lock{g_mutex};
            if (auto it = g_responders.find(responder.token); it != g_responders.end()) {
                consumed = std::move(it->second);
                g_responders.erase(it);
            }
        }
        revoke_handle(responder.token);
        if (out_body != nullptr) *out_body = publish_object(std::move(writer));
        return STURDY_OK;
    }
} // namespace

extern "C" {

// ----------------------------------------------------------------------------------------------------------------------
// Servers
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_http_server_create(const SturdyHttpServerOptions *options, SturdyHttpHandlerFn handler, void *user_data, SturdyAsyncObject *out_server) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_server, "output pointer must not be null");
        if (handler == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "handler must not be null");
        *out_server = publish_object(B::http_server_create(server_spec_of(options), adapt(handler, user_data)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_start(const SturdyHttpServerOptions *options, SturdyHttpHandlerFn handler, void *user_data, SturdyAsyncObject *out_server) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_server, "output pointer must not be null");
        if (handler == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "handler must not be null");
        *out_server = publish_object(B::http_server_start(server_spec_of(options), adapt(handler, user_data)));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_create_router(const SturdyHttpServerOptions *options, SturdyAsyncObject router, SturdyAsyncObject *out_server) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_server, "output pointer must not be null");
        STURDY_WITH_OBJECT(router, object);
        *out_server = publish_object(B::http_server_create_router(server_spec_of(options), object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_start_router(const SturdyHttpServerOptions *options, SturdyAsyncObject router, SturdyAsyncObject *out_server) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_server, "output pointer must not be null");
        STURDY_WITH_OBJECT(router, object);
        *out_server = publish_object(B::http_server_start_router(server_spec_of(options), object));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_on_websocket(SturdyAsyncObject server, const char *path, SturdyWebSocketFn handler, void *user_data) {
    return run([&]() -> SturdyResult {
        if (path == nullptr || handler == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "path and handler must not be null");
        STURDY_WITH_OBJECT(server, object);
        B::http_server_on_websocket(object, path, [handler, user_data](B::ObjectPtr socket, B::HttpRequestView request) {
            const SturdyAsyncObject handle = publish_object(std::move(socket));
            no_throw([&] { with_request(request, [&](const SturdyHttpRequest &view) { handler(user_data, handle, &view); }); });
        });
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_begin(SturdyAsyncObject server) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(server, object);
        B::http_server_begin(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_respond(SturdyHttpResponder responder, uint32_t status, const SturdyKeyValue *headers, uint32_t header_count, const uint8_t *body, size_t body_size) {
    return run([&]() -> SturdyResult {
        B::HttpResponseSpec response;
        response.status = status;
        response.headers = fields_of(headers, header_count);
        response.body = bytes_of(body, body_size);
        return send_response(responder, std::move(response), nullptr);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_respond_ex(SturdyHttpResponder responder, const SturdyHttpResponse *response, SturdyAsyncObject *out_body) {
    return run([&]() -> SturdyResult {
        if (response == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "response must not be null");
        const bool streamed = response->stream != STURDY_FALSE || response->sse != STURDY_FALSE;
        if (streamed && out_body == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a streamed response needs out_body");
        B::HttpResponseSpec spec;
        spec.status = response->status;
        spec.headers = fields_of(response->headers, response->header_count);
        spec.body = bytes_of(response->body, response->body_size);
        spec.keep_alive = response->close_connection == STURDY_FALSE;
        spec.body_file = text(response->body_file);
        spec.body_offset = response->body_offset;
        spec.body_length = response->limit_body_length != STURDY_FALSE ? static_cast<std::int64_t>(response->body_length) : -1;
        spec.stream = response->stream != STURDY_FALSE;
        spec.sse = response->sse != STURDY_FALSE;
        if (response->stream_queue != 0) spec.stream_queue = response->stream_queue;
        return send_response(responder, std::move(spec), out_body);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_respond_canned(SturdyHttpResponder responder, SturdyHttpResponseKind kind, uint32_t status, const char *body) {
    return run([&]() -> SturdyResult {
        if (kind < STURDY_HTTP_RESPONSE_TEXT || kind > STURDY_HTTP_RESPONSE_REDIRECT) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unknown response kind");
        return send_response(responder, B::canned_response(static_cast<B::ResponseKind>(kind), status, text(body)), nullptr);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_port(SturdyAsyncObject server, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_port, "output pointer must not be null");
        STURDY_WITH_OBJECT(server, object);
        *out_port = B::http_server_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_http3_port(SturdyAsyncObject server, uint16_t *out_port) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_port, "output pointer must not be null");
        STURDY_WITH_OBJECT(server, object);
        *out_port = B::http_server_http3_port(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_info(SturdyAsyncObject server, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(server, object);
        return publish_fields(B::http_server_info(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_metrics(SturdyAsyncObject server, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(server, object);
        return publish_fields(B::http_server_metrics(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_server_stop(SturdyAsyncObject server) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(server, object);
        B::http_server_stop(object);
        return STURDY_OK;
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Streamed bodies and helpers
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_http_body_write(SturdyAsyncObject body, const uint8_t *data, size_t size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(body, object);
        return publish_op(B::http_body_write(object, bytes_of(data, size)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_body_write_sse(SturdyAsyncObject body, const SturdySseEvent *event, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (event == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "event must not be null");
        STURDY_WITH_OBJECT(body, object);
        return publish_op(B::http_body_write_sse(object, sse_of(*event)), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_body_finish(SturdyAsyncObject body) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(body, object);
        B::http_body_finish(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_body_closed(SturdyAsyncObject body, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(body, object);
        return publish_op(B::http_body_closed(object), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_format_sse(const SturdySseEvent *event, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        if (event == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "event must not be null");
        return SFT::Ffi::copy_string_out(B::format_sse(sse_of(*event)), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_percent_decode(const char *text_in, SturdyBool plus_is_space, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        if (text_in == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "text must not be null");
        return SFT::Ffi::copy_string_out(B::percent_decode(text_in, plus_is_space != STURDY_FALSE), buffer, capacity, out_length);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Router
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_router_create(SturdyAsyncObject *out_router) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_router, "output pointer must not be null");
        *out_router = publish_object(B::router_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_router_route(SturdyAsyncObject router, const char *method, const char *pattern, SturdyHttpHandlerFn handler, void *user_data) {
    return run([&]() -> SturdyResult {
        if (method == nullptr || pattern == nullptr || handler == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "method, pattern and handler must not be null");
        STURDY_WITH_OBJECT(router, object);
        B::router_route(object, method, pattern, adapt(handler, user_data));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_router_use(SturdyAsyncObject router, SturdyHttpHandlerFn middleware, void *user_data) {
    return run([&]() -> SturdyResult {
        if (middleware == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "middleware must not be null");
        STURDY_WITH_OBJECT(router, object);
        B::router_use(object, adapt(middleware, user_data, true));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_router_static_files(SturdyAsyncObject router, const char *mount, const SturdyStaticFilesOptions *options) {
    return run([&]() -> SturdyResult {
        if (mount == nullptr || options == nullptr || options->root == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "mount and a root directory are required");
        STURDY_WITH_OBJECT(router, object);
        B::StaticFilesSpec spec;
        spec.root = options->root;
        if (options->index_file != nullptr) spec.index_file = options->index_file;
        if (options->cache_control != nullptr) spec.cache_control = options->cache_control;
        spec.allow_dotfiles = options->allow_dotfiles != STURDY_FALSE;
        spec.follow_symlinks = options->follow_symlinks != STURDY_FALSE;
        spec.serve_precompressed = options->no_precompressed == STURDY_FALSE;
        spec.spa_fallback = text(options->spa_fallback);
        spec.mime_types = fields_of(options->mime_types, options->mime_type_count);
        B::router_static_files(object, mount, spec);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_router_not_found(SturdyAsyncObject router, SturdyHttpHandlerFn handler, void *user_data) {
    return run([&]() -> SturdyResult {
        if (handler == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "handler must not be null");
        STURDY_WITH_OBJECT(router, object);
        B::router_not_found(object, adapt(handler, user_data));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_router_rate_limit(SturdyAsyncObject router, double rate_per_second, double burst, uint64_t max_keys) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(router, object);
        B::router_rate_limit(object, rate_per_second, burst, max_keys != 0 ? max_keys : 10000);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_router_dispatch(SturdyAsyncObject router, const char *method, const char *target, const char *remote_address, const SturdyKeyValue *headers,
                                                   uint32_t header_count, const uint8_t *body, size_t body_size, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        if (method == nullptr || target == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "method and target must not be null");
        STURDY_WITH_OBJECT(router, object);
        B::HttpRequestView request;
        request.method = method;
        request.target = target;
        request.remote_address = text(remote_address);
        request.headers = fields_of(headers, header_count);
        request.body = bytes_of(body, body_size);
        B::HttpResponseSpec response = B::router_dispatch(object, std::move(request));
        auto op = std::make_shared<B::Op>();
        B::Value value;
        value.status = B::OpStatus::Ok;
        value.number = response.status;
        value.fields = std::move(response.headers);
        value.bytes = std::move(response.body);
        op->complete(std::move(value));
        return publish_op(std::move(op), out_op);
    });
}

// ----------------------------------------------------------------------------------------------------------------------
// Rate limiter, cookie jar
// ----------------------------------------------------------------------------------------------------------------------
SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_create(double rate_per_second, double burst, uint64_t max_keys, SturdyAsyncObject *out_limiter) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_limiter, "output pointer must not be null");
        *out_limiter = publish_object(B::rate_limiter_new(rate_per_second, burst, max_keys != 0 ? max_keys : 10000));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_try_acquire(SturdyAsyncObject limiter, const char *key, double cost, SturdyBool *out_acquired) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_acquired, "output pointer must not be null");
        STURDY_WITH_OBJECT(limiter, object);
        *out_acquired = B::rate_limiter_try_acquire(object, text(key), cost) ? STURDY_TRUE : STURDY_FALSE;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_retry_after_ms(SturdyAsyncObject limiter, const char *key, double cost, int64_t *out_ms) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_ms, "output pointer must not be null");
        STURDY_WITH_OBJECT(limiter, object);
        *out_ms = B::rate_limiter_retry_after_ms(object, text(key), cost);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_rate_limiter_tracked_keys(SturdyAsyncObject limiter, uint64_t *out_keys) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_keys, "output pointer must not be null");
        STURDY_WITH_OBJECT(limiter, object);
        *out_keys = B::rate_limiter_tracked_keys(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_create(SturdyAsyncObject *out_jar) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_jar, "output pointer must not be null");
        *out_jar = publish_object(B::cookie_jar_new());
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_add(SturdyAsyncObject jar, const SturdyCookie *cookie) {
    return run([&]() -> SturdyResult {
        if (cookie == nullptr || cookie->name == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "a cookie with a name is required");
        STURDY_WITH_OBJECT(jar, object);
        B::CookieSpec spec;
        spec.name = cookie->name;
        spec.value = text(cookie->value);
        spec.domain = text(cookie->domain);
        if (cookie->path != nullptr) spec.path = cookie->path;
        spec.host_only = cookie->domain_wide == STURDY_FALSE;
        spec.secure = cookie->secure != STURDY_FALSE;
        spec.http_only = cookie->http_only != STURDY_FALSE;
        spec.expires_unix = cookie->expires_unix > 0 ? cookie->expires_unix : -1;
        B::cookie_jar_add(object, spec);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_store_from_response(SturdyAsyncObject jar, const char *host, const char *path, SturdyBool https, const SturdyKeyValue *response_headers,
                                                                  uint32_t header_count) {
    return run([&]() -> SturdyResult {
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        STURDY_WITH_OBJECT(jar, object);
        B::cookie_jar_store_from_response(object, host, path != nullptr ? path : "/", https != STURDY_FALSE, fields_of(response_headers, header_count));
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_header_for(SturdyAsyncObject jar, const char *host, const char *path, SturdyBool https, char *buffer, size_t capacity, size_t *out_length) {
    return run([&]() -> SturdyResult {
        if (host == nullptr) return set_error(STURDY_ERROR_INVALID_ARGUMENT, "host must not be null");
        STURDY_WITH_OBJECT(jar, object);
        return SFT::Ffi::copy_string_out(B::cookie_jar_header_for(object, host, path != nullptr ? path : "/", https != STURDY_FALSE), buffer, capacity, out_length);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_all(SturdyAsyncObject jar, SturdyAsyncOp *out_op) {
    return run([&]() -> SturdyResult {
        STURDY_OP_OUT(out_op);
        STURDY_WITH_OBJECT(jar, object);
        B::Fields fields;
        std::size_t index = 0;
        for (const auto &c : B::cookie_jar_all(object)) {
            const std::string n = std::to_string(index++);
            fields.emplace_back("name:" + n, c.name);
            fields.emplace_back("value:" + n, c.value);
            fields.emplace_back("domain:" + n, c.domain);
            fields.emplace_back("path:" + n, c.path);
            fields.emplace_back("domain_wide:" + n, c.host_only ? "0" : "1");
            fields.emplace_back("secure:" + n, c.secure ? "1" : "0");
            fields.emplace_back("http_only:" + n, c.http_only ? "1" : "0");
            fields.emplace_back("expires:" + n, c.expires_unix >= 0 ? std::to_string(c.expires_unix) : "0");
        }
        return publish_fields(std::move(fields), out_op);
    });
}

SturdyResult STURDY_ABI_CALL sturdy_cookie_jar_clear(SturdyAsyncObject jar) {
    return run([&]() -> SturdyResult {
        STURDY_WITH_OBJECT(jar, object);
        B::cookie_jar_clear(object);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_http_client_cookie_jar(SturdyAsyncObject client, SturdyAsyncObject *out_jar) {
    return run([&]() -> SturdyResult {
        STURDY_REQUIRE(out_jar, "output pointer must not be null");
        STURDY_WITH_OBJECT(client, object);
        auto jar = B::http_client_cookie_jar(object);
        if (!jar) return set_error(STURDY_ERROR_NOT_AVAILABLE, "the client has cookies turned off");
        *out_jar = publish_object(std::move(jar));
        return STURDY_OK;
    });
}

} // extern "C"
