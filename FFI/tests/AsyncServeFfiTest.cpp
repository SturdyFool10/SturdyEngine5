/// Drives the second half of the async C ABI through the public header only: the HTTP router, static files, streamed and SSE bodies,
/// WebSocket upgrades on the HTTP server, the rate limiter and cookie jar, cancellation, channels and synchronisation primitives,
/// tasks, streams, file-system helpers, generic byte streams and the extra data-parallel operations. Needs no engine session or GPU.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <FFI/Sturdy.h>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "AsyncServeFfiTest: %s\n", description);
            ++failures;
        }
    }

    // ---- helpers over the C API ---------------------------------------------------------------------------------------
    bool wait(SturdyAsyncOp op, int64_t timeout_ms = 10000) {
        SturdyBool done = STURDY_FALSE;
        return sturdy_op_wait(op, timeout_ms, &done) == STURDY_OK && done != STURDY_FALSE;
    }
    SturdyOpStatus status(SturdyAsyncOp op) {
        SturdyOpStatus s = STURDY_OP_PENDING;
        (void)sturdy_op_status(op, &s);
        return s;
    }
    bool ok(SturdyAsyncOp op) { return wait(op) && status(op) == STURDY_OP_OK; }
    int64_t number(SturdyAsyncOp op) {
        int64_t n = -1;
        (void)sturdy_op_number(op, &n);
        return n;
    }
    std::string op_text(SturdyAsyncOp op) {
        size_t length = 0;
        (void)sturdy_op_text(op, nullptr, 0, &length);
        std::string out(length, '\0');
        (void)sturdy_op_text(op, out.data(), out.size(), &length);
        if (!out.empty()) out.pop_back();
        return out;
    }
    std::string op_bytes(SturdyAsyncOp op) {
        size_t length = 0;
        (void)sturdy_op_bytes(op, nullptr, 0, &length);
        std::string out(length, '\0');
        (void)sturdy_op_bytes(op, reinterpret_cast<uint8_t *>(out.data()), out.size(), &length);
        return out;
    }
    std::string op_field(SturdyAsyncOp op, const char *name) {
        size_t length = 0;
        if (sturdy_op_field(op, name, nullptr, 0, &length) != STURDY_OK) return "<none>";
        std::string out(length, '\0');
        (void)sturdy_op_field(op, name, out.data(), out.size(), &length);
        if (!out.empty()) out.pop_back();
        return out;
    }
    SturdyAsyncObject object_of(SturdyAsyncOp op) {
        SturdyAsyncObject object{0};
        check(sturdy_op_object(op, &object) == STURDY_OK, "the op produced an object");
        return object;
    }
    const uint8_t *u8(const char *s) { return reinterpret_cast<const uint8_t *>(s); }

    template <typename Make>
    SturdyAsyncObject make_object(Make make) {
        SturdyAsyncOp op{0};
        if (make(&op) != STURDY_OK || !ok(op)) {
            (void)std::fprintf(stderr, "AsyncServeFfiTest: could not create an object\n");
            ++failures;
            return SturdyAsyncObject{0};
        }
        const SturdyAsyncObject object = object_of(op);
        (void)sturdy_op_release(op);
        return object;
    }
    /// Runs an op to completion and returns its bytes (the op is released).
    template <typename Start>
    std::string bytes_of_op(Start start) {
        SturdyAsyncOp op{0};
        if (start(&op) != STURDY_OK || !ok(op)) {
            ++failures;
            return "<failed>";
        }
        std::string out = op_bytes(op);
        (void)sturdy_op_release(op);
        return out;
    }
    template <typename Start>
    bool finishes(Start start) {
        SturdyAsyncOp op{0};
        const bool good = start(&op) == STURDY_OK && ok(op);
        (void)sturdy_op_release(op);
        return good;
    }
    std::string unpack(const std::string &packed, uint32_t index) {
        size_t length = 0;
        if (sturdy_frames_get(u8(packed.data()), packed.size(), index, nullptr, 0, &length) != STURDY_OK) return "<none>";
        std::string out(length, '\0');
        (void)sturdy_frames_get(u8(packed.data()), packed.size(), index, reinterpret_cast<uint8_t *>(out.data()), out.size(), &length);
        return out;
    }
    uint32_t frame_count(const std::string &packed) {
        uint32_t n = 0;
        return sturdy_frames_count(u8(packed.data()), packed.size(), &n) == STURDY_OK ? n : 9999;
    }

    // ---- HTTP handlers -------------------------------------------------------------------------------------------------
    void reply_text(SturdyHttpResponder responder, uint32_t status_code, const std::string &text) {
        (void)sturdy_http_respond(responder, status_code, nullptr, 0, u8(text.data()), text.size());
    }
    void user_route(void *, const SturdyHttpRequest *request, SturdyHttpResponder responder) {
        std::string id;
        for (uint32_t i = 0; i < request->param_count; ++i) if (std::string(request->params[i].name) == "id") id = request->params[i].value;
        reply_text(responder, 200, "user " + id + " path=" + request->path + " query=" + request->query);
    }
    void deny_middleware(void *, const SturdyHttpRequest *request, SturdyHttpResponder responder) {
        for (uint32_t i = 0; i < request->header_count; ++i) {
            if (std::string(request->headers[i].name) == "X-Deny") reply_text(responder, 403, "denied");
        }
    }
    void not_found(void *, const SturdyHttpRequest *request, SturdyHttpResponder responder) { reply_text(responder, 404, std::string("nothing at ") + request->path); }

    void stream_handler(void *, const SturdyHttpRequest *request, SturdyHttpResponder responder) {
        const std::string target = request->target;
        SturdyHttpResponse response{};
        response.status = 200;
        SturdyAsyncObject body{0};
        if (target == "/sse") {
            response.sse = STURDY_TRUE;
            (void)sturdy_http_respond_ex(responder, &response, &body);
            SturdySseEvent event{};
            event.data = "line1\nline2";
            event.event = "tick";
            event.retry_ms = 1500;
            event.has_retry = STURDY_TRUE;
            SturdyAsyncOp op{0};
            (void)sturdy_http_body_write_sse(body, &event, &op);
            (void)wait(op);
            (void)sturdy_op_release(op);
        } else if (target == "/file") {
            for (uint32_t i = 0; i < request->header_count; ++i) if (std::string(request->headers[i].name) == "X-File") response.body_file = request->headers[i].value;
            response.body_offset = 2;
            response.body_length = 5;
            response.limit_body_length = STURDY_TRUE;
            (void)sturdy_http_respond_ex(responder, &response, nullptr);
            return;
        } else {
            response.stream = STURDY_TRUE;
            (void)sturdy_http_respond_ex(responder, &response, &body);
            for (const char *piece : {"one,", "two,", "three"}) {
                SturdyAsyncOp op{0};
                (void)sturdy_http_body_write(body, u8(piece), std::strlen(piece), &op);
                (void)wait(op);
                (void)sturdy_op_release(op);
            }
        }
        (void)sturdy_http_body_finish(body);
        (void)sturdy_object_release(body);
    }

    std::atomic<int> websocket_calls{0};
    void echo_socket(void *, SturdyAsyncObject socket, const SturdyHttpRequest *request) {
        if (std::string(request->target) == "/ws?room=1") ++websocket_calls;
        SturdyAsyncOp op{0};
        (void)sturdy_websocket_receive(socket, &op);
        if (ok(op)) {
            const std::string message = "echo:" + op_bytes(op);
            SturdyAsyncOp send{0};
            (void)sturdy_websocket_send_text(socket, message.data(), message.size(), &send);
            (void)wait(send);
            (void)sturdy_op_release(send);
        }
        (void)sturdy_op_release(op);
        SturdyAsyncOp close{0};
        (void)sturdy_websocket_close(socket, 1000, "bye", &close);
        (void)wait(close);
        (void)sturdy_op_release(close);
        (void)sturdy_object_release(socket);
    }

    // ---- task and stream callbacks ---------------------------------------------------------------------------------------
    void constant_work(void *user_data, SturdyAsyncObject, SturdyTaskOutput *output) {
        const int64_t value = *static_cast<int64_t *>(user_data);
        const std::string text = "task-" + std::to_string(value);
        (void)sturdy_task_output_set(output, value, u8(text.data()), text.size(), "done", nullptr);
    }
    void failing_work(void *, SturdyAsyncObject, SturdyTaskOutput *output) { (void)sturdy_task_output_set(output, 0, nullptr, 0, nullptr, "boom"); }
    void cooperative_work(void *user_data, SturdyAsyncObject token, SturdyTaskOutput *output) {
        for (int i = 0; i < 400; ++i) {
            SturdyBool cancelled = STURDY_FALSE;
            (void)sturdy_cancel_token_is_cancelled(token, &cancelled);
            if (cancelled != STURDY_FALSE) {
                static_cast<std::atomic<bool> *>(user_data)->store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        (void)sturdy_task_output_set(output, 0, nullptr, 0, nullptr, nullptr);
    }
    void upper_map(void *, const uint8_t *data, size_t size, SturdyBytesSink *out) {
        std::string text(reinterpret_cast<const char *>(data), size);
        for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        (void)sturdy_bytes_sink_set(out, u8(text.data()), text.size());
    }
    SturdyBool odd_only(void *, const uint8_t *data, size_t size) { return size == 1 && ((data[0] - '0') % 2 == 1) ? STURDY_TRUE : STURDY_FALSE; }
    void fold_concat(void *, const uint8_t *acc, size_t acc_size, const uint8_t *item, size_t item_size, SturdyBytesSink *out) {
        std::string joined(reinterpret_cast<const char *>(acc), acc_size);
        joined.append(reinterpret_cast<const char *>(item), item_size);
        (void)sturdy_bytes_sink_set(out, u8(joined.data()), joined.size());
    }
    void count_each(void *user_data, const uint8_t *, size_t) { ++*static_cast<std::atomic<int> *>(user_data); }
    SturdyBool source_three(void *user_data, SturdyBytesSink *out) {
        int &produced = *static_cast<int *>(user_data);
        if (produced >= 3) return STURDY_FALSE;
        const std::string item = std::to_string(produced++);
        (void)sturdy_bytes_sink_set(out, u8(item.data()), item.size());
        return STURDY_TRUE;
    }
    void bump(void *user_data) { ++*static_cast<std::atomic<int> *>(user_data); }
    void scope_body(void *user_data, SturdyAsyncObject scope) {
        for (int i = 0; i < 20; ++i) (void)sturdy_par_scope_spawn(scope, bump, user_data);
    }
    SturdyBool divisible_by_three(void *, uint64_t index) { return index % 3 == 0 ? STURDY_TRUE : STURDY_FALSE; }
    int64_t index_value(void *, uint64_t index) { return static_cast<int64_t>(index); }
    int64_t add_i64(void *, int64_t a, int64_t b) { return a + b; }
    SturdyBool int_less(void *, const void *a, const void *b) { return *static_cast<const int *>(a) < *static_cast<const int *>(b) ? STURDY_TRUE : STURDY_FALSE; }
    void window_hit(void *user_data, uint64_t) { ++*static_cast<std::atomic<int> *>(user_data); }
    void destroy_counter(void *pointer) { ++*static_cast<std::atomic<int> *>(pointer); }
    void then_double(void *, SturdyAsyncObject, const SturdyTaskInput *input, SturdyTaskOutput *output) {
        (void)sturdy_task_output_set(output, input->number * 2, nullptr, 0, input->text, nullptr);
    }
    void then_fail_if_nine(void *, SturdyAsyncObject, const SturdyTaskInput *input, SturdyTaskOutput *output) {
        if (input->number == 9) (void)sturdy_task_output_set(output, 0, nullptr, 0, nullptr, "nine is refused");
        else (void)sturdy_task_output_set(output, input->number, nullptr, 0, nullptr, nullptr);
    }
    SturdyBool authorize_prefix(void *, const char *token, const char *) { return std::strncmp(token, "allowed-", 8) == 0 ? STURDY_TRUE : STURDY_FALSE; }

} // anonymous namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path scratch = fs::temp_directory_path() / "sturdy_ffi_serve_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch / "site");
    { std::ofstream(scratch / "site" / "index.html") << "<h1>home</h1>"; }
    { std::ofstream(scratch / "range.bin") << "0123456789"; }
    const std::string root = scratch.string();

    // ---- router, static files, helpers ----
    SturdyAsyncObject router{0};
    check(sturdy_router_create(&router) == STURDY_OK, "router");
    check(sturdy_router_use(router, deny_middleware, nullptr) == STURDY_OK, "middleware");
    check(sturdy_router_route(router, "GET", "/users/:id", user_route, nullptr) == STURDY_OK, "route");
    check(sturdy_router_not_found(router, not_found, nullptr) == STURDY_OK, "not_found");
    const std::string site = root + "/site";
    SturdyStaticFilesOptions files{};
    files.root = site.c_str();
    check(sturdy_router_static_files(router, "/static", &files) == STURDY_OK, "static files");
    SturdyStaticFilesOptions bad_files{};
    bad_files.root = "/definitely/not/a/directory";
    check(sturdy_router_static_files(router, "/x", &bad_files) == STURDY_ERROR_INVALID_ARGUMENT, "a bad static root is refused");
    {
        SturdyAsyncOp op{0};
        check(sturdy_router_dispatch(router, "GET", "/users/42?x=1", "", nullptr, 0, nullptr, 0, &op) == STURDY_OK && ok(op), "dispatch");
        check(number(op) == 200 && op_bytes(op) == "user 42 path=/users/42 query=x=1", "dispatch matches :id and splits the query");
        (void)sturdy_op_release(op);
        check(sturdy_router_dispatch(router, "GET", "/nowhere", "", nullptr, 0, nullptr, 0, &op) == STURDY_OK && number(op) == 404 && op_bytes(op) == "nothing at /nowhere", "custom not_found");
        (void)sturdy_op_release(op);
    }
    SturdyHttpServerOptions server_options{};
    server_options.bind_address = "127.0.0.1";
    SturdyAsyncObject server{0};
    check(sturdy_http_server_start_router(&server_options, router, &server) == STURDY_OK, "server starts on the router");
    uint16_t port = 0;
    check(sturdy_http_server_port(server, &port) == STURDY_OK && port != 0, "port");
    const std::string base = "http://127.0.0.1:" + std::to_string(port);
    SturdyAsyncObject http{0};
    check(sturdy_http_client_create(nullptr, &http) == STURDY_OK, "client");
    auto request = [&](const std::string &method, const std::string &url, std::vector<SturdyKeyValue> headers = {}) {
        SturdyAsyncOp op{0};
        const SturdyResult r = sturdy_http_client_request(http, method.c_str(), url.c_str(), headers.data(), static_cast<uint32_t>(headers.size()), nullptr, 0, &op);
        check(r == STURDY_OK && wait(op), "request finished");
        return op;
    };
    {
        SturdyAsyncOp r = request("GET", base + "/users/7?q=1");
        check(number(r) == 200 && op_bytes(r) == "user 7 path=/users/7 query=q=1", "a routed request over the wire");
        (void)sturdy_op_release(r);
        r = request("GET", base + "/users/7", {{"X-Deny", "1"}});
        check(number(r) == 403 && op_bytes(r) == "denied", "middleware answers before the route");
        (void)sturdy_op_release(r);
        r = request("GET", base + "/static/index.html");
        check(number(r) == 200 && op_bytes(r) == "<h1>home</h1>", "static file");
        (void)sturdy_op_release(r);
    }
    {
        char buffer[64];
        size_t length = 0;
        check(sturdy_http_percent_decode("a%20b+c", STURDY_TRUE, buffer, sizeof(buffer), &length) == STURDY_OK && std::string(buffer) == "a b c", "percent_decode");
        check(sturdy_http_percent_decode("%zz", STURDY_FALSE, buffer, sizeof(buffer), &length) == STURDY_ERROR_INVALID_ARGUMENT, "a malformed escape is refused");
        SturdySseEvent event{};
        event.data = "hello";
        event.event = "a\nb";
        check(sturdy_http_format_sse(&event, buffer, sizeof(buffer), &length) == STURDY_OK && std::string(buffer) == "event: ab\ndata: hello\n\n", "format_sse drops line breaks in the event name");
    }
    check(sturdy_http_server_stop(server) == STURDY_OK, "server stops");
    (void)sturdy_object_release(server);

    // ---- streamed, SSE and file bodies; WebSocket on the HTTP server ----
    {
        SturdyHttpServerOptions options{};
        options.bind_address = "127.0.0.1";
        options.websocket_subprotocols = nullptr;
        SturdyAsyncObject srv{0};
        check(sturdy_http_server_create(&options, stream_handler, nullptr, &srv) == STURDY_OK, "create (not started)");
        check(sturdy_http_server_on_websocket(srv, "/ws", echo_socket, nullptr) == STURDY_OK, "register a WebSocket path");
        check(sturdy_http_server_begin(srv) == STURDY_OK, "begin");
        SturdyAsyncOp info{0};
        check(sturdy_http_server_info(srv, &info) == STURDY_OK && op_field(info, "running") == "1", "info says running");
        (void)sturdy_op_release(info);
        uint16_t p = 0;
        (void)sturdy_http_server_port(srv, &p);
        const std::string b = "http://127.0.0.1:" + std::to_string(p);

        SturdyAsyncOp r = request("GET", b + "/stream");
        check(number(r) == 200 && op_bytes(r) == "one,two,three", "a streamed body arrives whole");
        (void)sturdy_op_release(r);
        r = request("GET", b + "/sse");
        check(op_field(r, "Content-Type") == "text/event-stream" && op_bytes(r) == "event: tick\nretry: 1500\ndata: line1\ndata: line2\n\n", "SSE");
        (void)sturdy_op_release(r);
        const std::string range_path = root + "/range.bin";
        r = request("GET", b + "/file", {{"X-File", range_path.c_str()}});
        check(op_bytes(r) == "23456", "a file body honours offset and length");
        (void)sturdy_op_release(r);

        const std::string url = "ws://127.0.0.1:" + std::to_string(p) + "/ws?room=1";
        SturdyAsyncObject ws = make_object([&](SturdyAsyncOp *op) { return sturdy_websocket_connect(url.c_str(), nullptr, op); });
        check(ws.token != 0, "WebSocket upgrade on the HTTP server");
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_websocket_send_text(ws, "hello", 5, op); }), "send");
        check(bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_websocket_receive(ws, op); }) == "echo:hello", "the handler echoes");
        check(websocket_calls == 1, "the handler saw the upgrade target");
        SturdyAsyncOp closing{0};
        (void)sturdy_websocket_receive(ws, &closing);
        check(wait(closing) && status(closing) != STURDY_OP_OK, "the handler's close ends the stream");
        (void)sturdy_op_release(closing);
        (void)sturdy_object_release(ws);
        (void)sturdy_object_release(srv);
    }

    // ---- rate limiter, cookie jar ----
    {
        SturdyAsyncObject limiter{0};
        check(sturdy_rate_limiter_create(1.0, 2.0, 100, &limiter) == STURDY_OK, "limiter");
        SturdyBool a = STURDY_FALSE, b = STURDY_FALSE, c = STURDY_TRUE;
        (void)sturdy_rate_limiter_try_acquire(limiter, "k", 1, &a);
        (void)sturdy_rate_limiter_try_acquire(limiter, "k", 1, &b);
        (void)sturdy_rate_limiter_try_acquire(limiter, "k", 1, &c);
        check(a && b && !c, "the burst is allowed, the next refused");
        int64_t wait_ms = 0;
        check(sturdy_rate_limiter_retry_after_ms(limiter, "k", 1, &wait_ms) == STURDY_OK && wait_ms > 0, "retry_after");
        (void)sturdy_object_release(limiter);

        SturdyAsyncObject jar{0};
        check(sturdy_cookie_jar_create(&jar) == STURDY_OK, "jar");
        SturdyCookie cookie{};
        cookie.name = "sid";
        cookie.value = "abc";
        cookie.domain = "example.com";
        check(sturdy_cookie_jar_add(jar, &cookie) == STURDY_OK, "add");
        char buffer[64];
        size_t length = 0;
        check(sturdy_cookie_jar_header_for(jar, "example.com", "/", STURDY_FALSE, buffer, sizeof(buffer), &length) == STURDY_OK && std::string(buffer) == "sid=abc", "header_for");
        SturdyAsyncOp all{0};
        check(sturdy_cookie_jar_all(jar, &all) == STURDY_OK && op_field(all, "name:0") == "sid" && op_field(all, "domain_wide:0") == "0", "all");
        (void)sturdy_op_release(all);
        (void)sturdy_cookie_jar_clear(jar);
        (void)sturdy_object_release(jar);
        SturdyAsyncObject none{0};
        check(sturdy_http_client_cookie_jar(http, &none) == STURDY_ERROR_NOT_AVAILABLE, "a client without cookies has no jar");
    }

    // ---- cancellation ----
    {
        SturdyAsyncObject source{0}, token{0};
        check(sturdy_cancel_source_create(&source) == STURDY_OK && sturdy_cancel_source_token(source, &token) == STURDY_OK, "source and token");
        SturdyAsyncOp waiting{0}, sleeper{0};
        (void)sturdy_cancel_token_wait(token, &waiting);
        (void)sturdy_timer_sleep(60000, &sleeper);
        check(sturdy_op_link_cancel(sleeper, token) == STURDY_OK, "link");
        SturdyBool first = STURDY_FALSE, second = STURDY_TRUE;
        (void)sturdy_cancel_source_cancel(source, &first);
        (void)sturdy_cancel_source_cancel(source, &second);
        check(first && !second && ok(waiting), "cancel reports the first call and wakes waiters");
        check(wait(sleeper) && status(sleeper) == STURDY_OP_CANCELLED, "a linked op is cancelled");
        SturdyAsyncOp slow{0}, timed{0};
        (void)sturdy_timer_sleep(60000, &slow);
        check(sturdy_op_with_timeout(slow, 40, &timed) == STURDY_OK && wait(timed) && status(timed) == STURDY_OP_FAILED, "op_with_timeout fails");
        SturdyIoError code = STURDY_IO_NONE;
        (void)sturdy_op_error_code(timed, &code);
        check(code == STURDY_IO_TIMED_OUT, "with TimedOut");
        for (SturdyAsyncOp op : {waiting, sleeper, slow, timed}) (void)sturdy_op_release(op);
    }

    // ---- channels and sync ----
    {
        SturdyAsyncObject tx{0}, rx{0};
        check(sturdy_channel_create(2, &tx, &rx) == STURDY_OK, "channel");
        SturdyTryStatus st = STURDY_TRY_CLOSED;
        check(sturdy_channel_try_send(tx, u8("a"), 1, &st) == STURDY_OK && st == STURDY_TRY_OK, "try_send");
        (void)sturdy_channel_try_send(tx, u8("b"), 1, &st);
        (void)sturdy_channel_try_send(tx, u8("c"), 1, &st);
        check(st == STURDY_TRY_WOULD_BLOCK, "a full channel would block");
        check(bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_channel_recv(rx, op); }) == "a", "recv");
        uint8_t buffer[8];
        size_t length = 0;
        SturdyAsyncOp taken{0};
        check(sturdy_channel_try_recv(rx, &taken, &st) == STURDY_OK && st == STURDY_TRY_OK && op_bytes(taken) == "b", "try_recv");
        (void)sturdy_op_release(taken);
        check(sturdy_channel_try_recv(rx, &taken, &st) == STURDY_OK && st == STURDY_TRY_WOULD_BLOCK && taken.token == 0, "empty");
        (void)sturdy_object_release(tx);
        SturdyAsyncOp end{0};
        (void)sturdy_channel_recv(rx, &end);
        check(ok(end) && number(end) == 0, "releasing the last sender ends the stream");
        (void)sturdy_op_release(end);

        SturdyAsyncObject os{0}, orx{0};
        (void)sturdy_oneshot_create(&os, &orx);
        SturdyAsyncOp pending{0};
        (void)sturdy_oneshot_recv(orx, &pending);
        SturdyBool was_first = STURDY_FALSE;
        (void)sturdy_oneshot_send(os, u8("once"), 4, &was_first);
        check(was_first && ok(pending) && op_bytes(pending) == "once", "oneshot");
        (void)sturdy_op_release(pending);

        SturdyAsyncObject wtx{0}, wrx{0};
        (void)sturdy_watch_value_create(u8("v0"), 2, &wtx, &wrx);
        SturdyBool changed = STURDY_TRUE;
        (void)sturdy_watch_value_has_changed(wrx, &changed);
        check(!changed, "a watch starts unchanged");
        (void)sturdy_watch_value_send(wtx, u8("v1"), 2);
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_watch_value_changed(wrx, op); }), "changed");
        check(sturdy_watch_value_borrow_and_update(wrx, buffer, sizeof(buffer), &length) == STURDY_OK && length == 2 && buffer[1] == '1', "borrow");

        SturdyAsyncObject btx{0}, brx{0};
        (void)sturdy_broadcast_create(4, &btx, &brx);
        uint64_t receivers = 0;
        (void)sturdy_broadcast_send(btx, u8("hi"), 2, &receivers);
        SturdyAsyncOp got{0};
        (void)sturdy_broadcast_recv(brx, &got);
        check(receivers == 1 && ok(got) && number(got) == 0 && op_bytes(got) == "hi", "broadcast");
        (void)sturdy_op_release(got);

        SturdyAsyncObject semaphore{0};
        (void)sturdy_semaphore_create(1, &semaphore);
        SturdyAsyncObject permit{0};
        check(sturdy_semaphore_try_acquire(semaphore, 1, &permit) == STURDY_OK && permit.token != 0, "try_acquire");
        SturdyAsyncObject second_permit{1};
        check(sturdy_semaphore_try_acquire(semaphore, 1, &second_permit) == STURDY_OK && second_permit.token == 0, "none left: the zero handle");
        uint64_t available = 9;
        (void)sturdy_permit_release(permit);
        (void)sturdy_semaphore_available(semaphore, &available);
        check(available == 1, "release returns the permit");

        SturdyAsyncObject mutex{0};
        (void)sturdy_async_mutex_create(u8("start"), 5, &mutex);
        SturdyAsyncOp lock{0};
        (void)sturdy_async_mutex_lock(mutex, &lock);
        check(ok(lock), "lock");
        SturdyAsyncObject guard = object_of(lock);
        (void)sturdy_guard_set(guard, u8("changed"), 7);
        check(sturdy_guard_get(guard, reinterpret_cast<uint8_t *>(buffer), sizeof(buffer), &length) == STURDY_OK && length == 7, "guard get/set");
        SturdyAsyncObject busy{1};
        check(sturdy_async_mutex_try_lock(mutex, &busy) == STURDY_OK && busy.token == 0, "a held mutex refuses try_lock");
        (void)sturdy_guard_release(guard);
        check(sturdy_guard_get(guard, buffer, sizeof(buffer), &length) != STURDY_OK, "a released guard cannot be read");
        (void)sturdy_op_release(lock);

        SturdyAsyncObject notify{0}, barrier{0};
        (void)sturdy_notify_create(&notify);
        (void)sturdy_notify_one(notify);
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_notify_notified(notify, op); }), "a stored notification is consumed");
        (void)sturdy_barrier_create(2, &barrier);
        SturdyAsyncOp w1{0}, w2{0};
        (void)sturdy_barrier_wait(barrier, &w1);
        (void)sturdy_barrier_wait(barrier, &w2);
        check(ok(w1) && ok(w2) && number(w1) + number(w2) == 1, "one party leads the barrier");
        (void)sturdy_op_release(w1);
        (void)sturdy_op_release(w2);
    }

    // ---- tasks ----
    {
        int64_t seven = 7;
        SturdyAsyncOp t{0};
        check(sturdy_task_spawn(constant_work, &seven, STURDY_LANE_HEAVY, &t) == STURDY_OK && ok(t) && number(t) == 7 && op_bytes(t) == "task-7" && op_text(t) == "done", "task_spawn");
        (void)sturdy_op_release(t);
        check(sturdy_task_spawn(failing_work, nullptr, STURDY_LANE_BLOCKING, &t) == STURDY_OK && wait(t) && status(t) == STURDY_OP_FAILED, "a task error fails the op");
        (void)sturdy_op_release(t);
        std::atomic<bool> saw_cancel{false};
        check(sturdy_task_spawn(cooperative_work, &saw_cancel, STURDY_LANE_LIGHT, &t) == STURDY_OK, "cooperative task");
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        (void)sturdy_op_cancel(t);
        (void)wait(t);
        check(saw_cancel.load(), "cancelling the op sets the token the task polls");
        (void)sturdy_op_release(t);

        SturdyAsyncObject set{0};
        (void)sturdy_join_set_create(&set);
        int64_t values[3] = {1, 2, 3};
        for (int64_t &v : values) (void)sturdy_join_set_spawn(set, constant_work, &v, STURDY_LANE_LIGHT);
        (void)sturdy_join_set_spawn(set, failing_work, nullptr, STURDY_LANE_LIGHT);
        int64_t sum = 0;
        int failed = 0;
        for (int i = 0; i < 4; ++i) {
            SturdyAsyncOp next{0};
            (void)sturdy_join_set_join_next(set, &next);
            (void)wait(next);
            if (op_field(next, "outcome") == "ok") sum += number(next);
            else if (op_field(next, "outcome") == "failed") ++failed;
            (void)sturdy_op_release(next);
        }
        check(sum == 6 && failed == 1, "a join set yields every task");

        SturdyAsyncObject local{0};
        (void)sturdy_local_set_create(&local);
        int64_t nine = 9;
        SturdyAsyncOp lop{0};
        (void)sturdy_local_set_spawn(local, constant_work, &nine, &lop);
        check(!wait(lop, 40), "a local task waits for its thread to pump");
        check(sturdy_local_set_block_on(local, lop) == STURDY_OK && status(lop) == STURDY_OP_OK && number(lop) == 9, "block_on runs it here");
        (void)sturdy_op_release(lop);

        SturdyAsyncObject dedicated{0};
        (void)sturdy_dedicated_thread_create("ffi-test", &dedicated);
        int64_t five = 5;
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_dedicated_thread_run(dedicated, constant_work, &five, op); }), "a dedicated thread runs work");
        (void)sturdy_object_release(dedicated);

        SturdyAsyncObject task_local{0};
        (void)sturdy_task_local_create(&task_local);
        uint8_t buffer[16];
        size_t length = 0;
        check(sturdy_task_local_get(task_local, buffer, sizeof(buffer), &length) == STURDY_ERROR_NOT_AVAILABLE, "unbound outside a scope");
        struct Scope {
            SturdyAsyncObject local;
            bool found = false;
        } scope{task_local};
        auto in_scope = [](void *user_data) {
            auto *s = static_cast<Scope *>(user_data);
            uint8_t b[16];
            size_t n = 0;
            s->found = sturdy_task_local_get(s->local, b, sizeof(b), &n) == STURDY_OK && n == 3 && b[0] == 'a';
        };
        check(sturdy_task_local_scope(task_local, u8("abc"), 3, in_scope, &scope) == STURDY_OK && scope.found, "bound inside the scope");

        std::atomic<int> main_runs{0};
        check(sturdy_main_thread_run(bump, &main_runs) == STURDY_OK && sturdy_main_thread_pump() == STURDY_OK && main_runs == 1, "main-thread work runs when pumped");
        SturdyAsyncOp stats{0};
        check(sturdy_io_lane_stats(&stats) == STURDY_OK && op_field(stats, "capacity") != "<none>", "io lane stats");
        (void)sturdy_op_release(stats);
        size_t cores = 0;
        check(sturdy_topology_ranked_cores(STURDY_FALSE, nullptr, 0, &cores) == STURDY_OK && cores > 0, "topology");
        SturdyAsyncObject interval{0};
        (void)sturdy_interval_create(15, &interval);
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_interval_tick(interval, op); }) && finishes([&](SturdyAsyncOp *op) { return sturdy_interval_tick(interval, op); }), "interval ticks");
    }

    // ---- streams ----
    {
        auto items = [](std::initializer_list<const char *> list) {
            std::vector<SturdyBytesView> views;
            for (const char *s : list) views.push_back({u8(s), std::strlen(s)});
            SturdyAsyncObject stream{0};
            check(sturdy_stream_of_items(views.data(), static_cast<uint32_t>(views.size()), &stream) == STURDY_OK, "stream_of_items");
            return stream;
        };
        auto collect = [&](SturdyAsyncObject stream) {
            std::string packed = bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_stream_collect(stream, op); });
            std::string joined;
            for (uint32_t i = 0; i < frame_count(packed); ++i) joined += unpack(packed, i) + ",";
            return joined;
        };
        SturdyAsyncObject mapped{0};
        check(sturdy_stream_map(items({"a", "b"}), upper_map, nullptr, &mapped) == STURDY_OK && collect(mapped) == "A,B,", "stream_map");
        SturdyAsyncObject filtered{0};
        (void)sturdy_stream_filter(items({"1", "2", "3", "4"}), odd_only, nullptr, &filtered);
        check(collect(filtered) == "1,3,", "stream_filter");
        SturdyAsyncObject taken{0};
        (void)sturdy_stream_take(items({"x", "y", "z"}), 2, &taken);
        check(collect(taken) == "x,y,", "stream_take");
        SturdyAsyncObject chained{0};
        (void)sturdy_stream_chain(items({"p"}), items({"q"}), &chained);
        check(collect(chained) == "p,q,", "stream_chain");
        SturdyAsyncObject zipped{0};
        (void)sturdy_stream_zip(items({"1", "2"}), items({"a", "b"}), &zipped);
        const std::string z = bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_stream_collect(zipped, op); });
        check(frame_count(z) == 2 && unpack(unpack(z, 1), 0) == "2" && unpack(unpack(z, 1), 1) == "b", "stream_zip packs pairs");
        {
            SturdyAsyncOp fold{0};
            (void)sturdy_stream_fold(items({"x", "y", "z"}), u8(">"), 1, fold_concat, nullptr, &fold);
            check(ok(fold) && op_bytes(fold) == ">xyz", "stream_fold");
            (void)sturdy_op_release(fold);
            std::atomic<int> seen{0};
            check(finishes([&](SturdyAsyncOp *op) { return sturdy_stream_for_each(items({"1", "2", "3"}), count_each, &seen, op); }) && seen == 3, "stream_for_each");
            SturdyAsyncOp count{0};
            (void)sturdy_stream_count(items({"1", "2", "3", "4"}), &count);
            check(ok(count) && number(count) == 4, "stream_count");
            (void)sturdy_op_release(count);
            SturdyAsyncOp last{0};
            (void)sturdy_stream_last(items({"1", "9"}), &last);
            check(ok(last) && number(last) == 1 && op_bytes(last) == "9", "stream_last");
            (void)sturdy_op_release(last);

            SturdyAsyncObject numbers = items({"1", "2"});
            SturdyAsyncOp first{0};
            (void)sturdy_stream_next(numbers, &first);
            check(ok(first) && number(first) == 1 && op_bytes(first) == "1", "stream_next");
            (void)sturdy_op_release(first);
            SturdyAsyncObject consumed{0};
            (void)sturdy_stream_take(numbers, 1, &consumed);
            SturdyAsyncObject again{0};
            check(sturdy_stream_take(numbers, 1, &again) == STURDY_ERROR_INVALID_ARGUMENT, "a consumed stream cannot be used again");
            (void)sturdy_object_release(consumed);

            int produced = 0;
            SturdyAsyncObject generated{0};
            (void)sturdy_stream_generate(source_three, &produced, &generated);
            check(collect(generated) == "0,1,2,", "stream_generate");

            SturdyAsyncObject tx{0}, rx{0}, out_tx{0}, out_rx{0};
            (void)sturdy_channel_create(4, &tx, &rx);
            (void)sturdy_channel_create(4, &out_tx, &out_rx);
            SturdyAsyncObject from_channel{0};
            (void)sturdy_stream_from_channel(rx, &from_channel);
            SturdyAsyncOp forward{0};
            (void)sturdy_stream_forward(from_channel, out_tx, &forward);
            check(finishes([&](SturdyAsyncOp *op) { return sturdy_channel_send(tx, u8("q"), 1, op); }), "send into the channel");
            check(bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_channel_recv(out_rx, op); }) == "q", "a channel feeds a stream that feeds a channel");
            (void)sturdy_object_release(tx);
            check(ok(forward), "the forward finishes when the source ends");
            (void)sturdy_op_release(forward);
        }
    }

    // ---- file system, generic byte streams ----
    {
        const std::string file = root + "/a/b/one.txt";
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_fs_create_directories((root + "/a/b").c_str(), op); }), "create_directories");
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_fs_write_file(file.c_str(), u8("hello "), 6, STURDY_FALSE, op); }) &&
                  finishes([&](SturdyAsyncOp *op) { return sturdy_fs_write_file(file.c_str(), u8("world"), 5, STURDY_TRUE, op); }),
              "write and append");
        check(bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_fs_read_file(file.c_str(), 0, op); }) == "hello world", "read_file");
        check(bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_fs_read_range(file.c_str(), 6, 5, op); }) == "world", "read_range");
        SturdyAsyncOp size{0};
        (void)sturdy_fs_size(file.c_str(), &size);
        check(ok(size) && number(size) == 11, "size");
        (void)sturdy_op_release(size);
        const std::string listing = bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_fs_list((root + "/a/b").c_str(), 0, STURDY_FALSE, op); });
        check(frame_count(listing) == 1, "list_directory");
        SturdyAsyncObject iter{0};
        (void)sturdy_dir_iter_create((root + "/a/b").c_str(), STURDY_FALSE, &iter);
        SturdyAsyncOp entry{0};
        (void)sturdy_dir_iter_next(iter, &entry);
        check(ok(entry) && number(entry) == 1 && op_text(entry).find("one.txt") != std::string::npos, "dir_iter");
        (void)sturdy_op_release(entry);

        SturdyAsyncObject pipe_a{0}, pipe_b{0};
        check(sturdy_memory_pipe_create(4, &pipe_a, &pipe_b) == STURDY_OK, "memory pipe");
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_io_write_all(pipe_a, STURDY_PROCESS_STDOUT, u8("one\r\ntwo\n"), 9, op); }), "write into the pipe");
        SturdyAsyncObject reader{0};
        check(sturdy_buffered_reader_create(pipe_b, STURDY_PROCESS_STDOUT, 0, &reader) == STURDY_OK, "buffered reader");
        SturdyAsyncOp line{0};
        (void)sturdy_buffered_read_line(reader, 0, &line);
        check(ok(line) && number(line) == 1 && op_text(line) == "one", "read_line");
        (void)sturdy_op_release(line);
        check(bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_buffered_read_exact(reader, 3, op); }) == "two", "read_exact");

        SturdyAsyncObject memory{0};
        (void)sturdy_memory_writer_create(&memory);
        SturdyAsyncObject source{0};
        (void)sturdy_memory_reader_create(u8("copy me"), 7, &source);
        SturdyAsyncOp copy{0};
        check(sturdy_io_copy(source, STURDY_PROCESS_STDOUT, memory, STURDY_PROCESS_STDOUT, nullptr, &copy) == STURDY_OK && ok(copy) && number(copy) == 7, "io_copy");
        (void)sturdy_op_release(copy);
        uint8_t collected[16];
        size_t length = 0;
        check(sturdy_memory_writer_bytes(memory, collected, sizeof(collected), &length) == STURDY_OK && length == 7 && std::memcmp(collected, "copy me", 7) == 0, "the copy arrived");

        SturdyAsyncObject end_a{0}, end_b{0};
        (void)sturdy_memory_pipe_create(8, &end_a, &end_b);
        SturdyAsyncObject sender{0}, receiver{0};
        check(sturdy_framed_create(end_a, STURDY_FRAME_LENGTH_DELIMITED, 0, &sender) == STURDY_OK && sturdy_framed_create(end_b, STURDY_FRAME_LENGTH_DELIMITED, 0, &receiver) == STURDY_OK, "framed");
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_framed_send(sender, u8("frame"), 5, op); }) && bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_framed_next(receiver, op); }) == "frame",
              "framed send/next");
        SturdyAsyncObject not_a_stream{0};
        check(sturdy_buffered_reader_create(memory, STURDY_PROCESS_STDOUT, 0, &not_a_stream) != STURDY_OK, "a write-only object cannot be read");
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_fs_remove_file(file.c_str(), op); }) && finishes([&](SturdyAsyncOp *op) { return sturdy_fs_remove_directory((root + "/a/b").c_str(), op); }), "remove_file and remove_directory");
    }

    // ---- more data parallelism ----
    {
        std::atomic<int> joined{0};
        check(sturdy_par_join(bump, &joined, bump, &joined) == STURDY_OK && joined == 2, "par_join");
        std::atomic<int> spawned{0};
        check(sturdy_par_scope(scope_body, &spawned) == STURDY_OK && spawned == 20, "par_scope waits for what it spawned");
        std::atomic<int> windows{0};
        check(sturdy_par_for_windows(10, 4, window_hit, &windows) == STURDY_OK && windows == 7, "par_for_windows");
        uint64_t count = 0;
        int64_t index = 0;
        SturdyBool any = STURDY_FALSE;
        check(sturdy_par_count(1000, divisible_by_three, nullptr, &count) == STURDY_OK && count == 334, "par_count");
        check(sturdy_par_find_first(1000, divisible_by_three, nullptr, &index) == STURDY_OK && index == 0, "par_find_first");
        check(sturdy_par_any(1000, divisible_by_three, nullptr, &any) == STURDY_OK && any, "par_any");
        int64_t total = 0;
        check(sturdy_par_map_reduce_i64(1000, index_value, 0, add_i64, nullptr, &total) == STURDY_OK && total == 499500, "par_map_reduce_i64");
        std::vector<int> values(2000);
        for (size_t i = 0; i < values.size(); ++i) values[i] = static_cast<int>((i * 7919) % 1000);
        check(sturdy_par_sort_by(values.data(), values.size(), sizeof(int), STURDY_TRUE, int_less, nullptr) == STURDY_OK, "par_sort_by");
        bool sorted = true;
        for (size_t i = 1; i < values.size(); ++i) sorted = sorted && values[i - 1] <= values[i];
        check(sorted, "and the array is sorted");
        std::vector<uint32_t> big(1000, 3000000000u);
        SturdyNumericValue value{};
        SturdyBool has = STURDY_FALSE;
        check(sturdy_par_reduce(STURDY_NUMERIC_U32, STURDY_REDUCE_SUM, big.data(), big.size(), &value, &has) == STURDY_OK && has && value.u64 == 3000000000000ull, "par_reduce sums u32 in 64 bits");
        check(sturdy_par_reduce(STURDY_NUMERIC_F64, STURDY_REDUCE_MIN, nullptr, 0, &value, &has) == STURDY_OK && !has, "min of nothing has no value");
        uint32_t threads = 0;
        check(sturdy_par_num_threads(&threads) == STURDY_OK && threads >= 1, "par_num_threads");
    }

    // ---- file/udp/process options that grew ----
    {
        SturdyAsyncObject a = make_object([](SturdyAsyncOp *op) { return sturdy_udp_bind("127.0.0.1", 0, nullptr, op); });
        SturdyAsyncObject b = make_object([](SturdyAsyncOp *op) { return sturdy_udp_bind("127.0.0.1", 0, nullptr, op); });
        uint16_t pa = 0, pb = 0;
        (void)sturdy_udp_port(a, &pa);
        (void)sturdy_udp_port(b, &pb);
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_udp_connect(a, "127.0.0.1", pb, op); }) && finishes([&](SturdyAsyncOp *op) { return sturdy_udp_connect(b, "127.0.0.1", pa, op); }), "udp_connect");
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_udp_send(a, u8("hi"), 2, op); }) && bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_udp_receive(b, 100, op); }) == "hi", "udp_send on a connected socket");
        check(finishes([&](SturdyAsyncOp *op) { return sturdy_udp_set_multicast_ttl(a, 2, op); }), "multicast ttl");

        SturdyAsyncObject listener = make_object([](SturdyAsyncOp *op) { return sturdy_tcp_listen("127.0.0.1", 0, nullptr, 16, op); });
        char address[64];
        size_t length = 0;
        check(sturdy_tcp_listener_address(listener, address, sizeof(address), &length) == STURDY_OK && std::string(address).rfind("127.0.0.1:", 0) == 0, "listener address");

        SturdyProcessOptions process{};
        const char *args[] = {"-c", "echo out; exit 3"};
        process.program = "/bin/sh";
        process.args = args;
        process.arg_count = 2;
        process.stdin_mode = STURDY_STDIO_NULL;
        SturdyAsyncObject child{0};
        check(sturdy_process_spawn(&process, &child) == STURDY_OK, "spawn with stdin = null");
        int64_t pid = 0;
        check(sturdy_process_id(child, &pid) == STURDY_OK && pid > 0, "process id");
        check(bytes_of_op([&](SturdyAsyncOp *op) { return sturdy_io_read_to_end(child, STURDY_PROCESS_STDOUT, 0, op); }) == "out\n", "io_read_to_end from a process");
        SturdyAsyncOp wait_op{0};
        (void)sturdy_process_wait(child, &wait_op);
        check(ok(wait_op) && number(wait_op) == 3 && op_field(wait_op, "signal") == "0", "exit status");
        (void)sturdy_op_release(wait_op);

        SturdyRelayOptions relay_options{};
        relay_options.bind_address = "127.0.0.1";
        SturdyAsyncObject relay{0};
        check(sturdy_relay_server_start(&relay_options, authorize_prefix, nullptr, &relay) == STURDY_OK, "relay with an authorizer");
        uint16_t relay_port = 0;
        (void)sturdy_relay_server_port(relay, &relay_port);
        SturdyAsyncOp denied{0};
        (void)sturdy_relay_connect("127.0.0.1", relay_port, "forbidden", STURDY_TRUE, nullptr, 3000, &denied);
        check(wait(denied) && status(denied) == STURDY_OP_FAILED, "the authorizer can refuse a client");
        (void)sturdy_op_release(denied);
    }

    // ---- shared-state primitives ----
    {
        SturdyAsyncObject mutex{0}, guard{0}, busy{1};
        check(sturdy_sync_mutex_create(u8("v0"), 2, "ffi-test", &mutex) == STURDY_OK, "sync mutex");
        check(sturdy_sync_mutex_lock(mutex, &guard) == STURDY_OK && guard.token != 0, "lock");
        check(sturdy_sync_mutex_try_lock(mutex, &busy) == STURDY_OK && busy.token == 0, "a held lock refuses try_lock");
        uint8_t value[8];
        size_t length = 0;
        check(sturdy_guard_get(guard, value, sizeof(value), &length) == STURDY_OK && length == 2 && value[1] == '0', "guard get");
        (void)sturdy_guard_set(guard, u8("v1"), 2);
        (void)sturdy_guard_release(guard);
        check(sturdy_sync_mutex_try_lock(mutex, &guard) == STURDY_OK && guard.token != 0, "free again");
        (void)sturdy_guard_release(guard);

        SturdyAsyncObject ring{0};
        check(sturdy_ring_create(4, 20, &ring) == STURDY_OK, "ring");
        uint64_t max_item = 0, capacity = 0, size = 0;
        (void)sturdy_ring_max_item_bytes(ring, &max_item);
        (void)sturdy_ring_capacity(ring, &capacity);
        check(max_item == 32 && capacity == 4, "ring geometry");
        SturdyRingStatus st = STURDY_RING_FULL;
        check(sturdy_ring_try_push(ring, u8("a"), 1, &st) == STURDY_OK && st == STURDY_RING_OK, "push");
        std::vector<uint8_t> big(33, 'x');
        (void)sturdy_ring_try_push(ring, big.data(), big.size(), &st);
        check(st == STURDY_RING_TOO_LARGE, "too large");
        (void)sturdy_ring_try_push(ring, u8("b"), 1, &st);
        uint8_t slot[32];
        check(sturdy_ring_try_pop(ring, slot, 8, &length, &st) == STURDY_ERROR_BUFFER_TOO_SMALL, "a small buffer is refused before popping");
        check(sturdy_ring_try_pop(ring, slot, sizeof(slot), &length, &st) == STURDY_OK && st == STURDY_RING_OK && length == 1 && slot[0] == 'a', "pop");
        SturdyAsyncOp drained{0};
        check(sturdy_ring_drain(ring, 0, &drained) == STURDY_OK && number(drained) == 1 && unpack(op_bytes(drained), 0) == "b", "drain");
        (void)sturdy_op_release(drained);
        (void)sturdy_ring_size(ring, &size);
        check(size == 0, "empty after drain");

        std::atomic<int> freed{0};
        SturdyAsyncObject arc{0}, clone{0};
        check(sturdy_arc_create_foreign(&freed, destroy_counter, &arc) == STURDY_OK && sturdy_arc_clone(arc, &clone) == STURDY_OK, "foreign arc");
        uint64_t count = 0;
        void *pointer = nullptr;
        SturdyBool same = STURDY_FALSE;
        (void)sturdy_arc_use_count(arc, &count);
        (void)sturdy_arc_pointer(clone, &pointer);
        (void)sturdy_arc_ptr_eq(arc, clone, &same);
        check(count == 2 && pointer == &freed && same, "clones share the pointer");
        (void)sturdy_object_release(arc);
        check(freed == 0, "not freed while a clone lives");
        (void)sturdy_object_release(clone);
        check(freed == 1, "freed with the last reference");
        SturdyAsyncObject bytes_arc{0};
        (void)sturdy_arc_create(u8("blob"), 4, &bytes_arc);
        check(sturdy_arc_bytes(bytes_arc, slot, sizeof(slot), &length) == STURDY_OK && length == 4, "bytes arc");

        SturdyAsyncObject promise{0};
        SturdyAsyncOp future{0};
        check(sturdy_promise_create(&promise, &future) == STURDY_OK && status(future) == STURDY_OP_PENDING, "promise");
        std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            (void)sturdy_promise_set_value(promise, 7, u8("later"), 5, "txt", nullptr);
        }).detach();
        check(ok(future) && number(future) == 7 && op_bytes(future) == "later" && op_text(future) == "txt", "the future carries the value");
        SturdyBool first = STURDY_TRUE;
        (void)sturdy_promise_set_error(promise, STURDY_IO_NOT_FOUND, "x", &first);
        check(!first, "only the first completion wins");
        SturdyAsyncObject dropped{0};
        SturdyAsyncOp broken{0};
        (void)sturdy_promise_create(&dropped, &broken);
        (void)sturdy_object_release(dropped);
        SturdyIoError code = STURDY_IO_NONE;
        check(wait(broken) && status(broken) == STURDY_OP_FAILED && sturdy_op_error_code(broken, &code) == STURDY_OK && code == STURDY_IO_CLOSED, "a dropped promise fails its future with Closed");
        SturdyAsyncOp ready{0};
        check(sturdy_op_ready(3, nullptr, 0, "r", &ready) == STURDY_OK && status(ready) == STURDY_OP_OK && number(ready) == 3, "op_ready");

        int64_t three = 3;
        SturdyAsyncObject just{0}, doubled{0}, refused{0};
        check(sturdy_exec_just(constant_work, &three, STURDY_LANE_LIGHT, &just) == STURDY_OK && sturdy_exec_then(just, then_double, nullptr, STURDY_LANE_HEAVY, &doubled) == STURDY_OK, "build a chain");
        SturdyAsyncOp result{0};
        check(sturdy_exec_submit(doubled, &result) == STURDY_OK && ok(result) && number(result) == 6 && op_text(result) == "done", "the chain runs");
        SturdyAsyncOp again{0};
        check(sturdy_exec_sync_wait(doubled, &again) == STURDY_OK && status(again) == STURDY_OP_OK && number(again) == 6, "and runs again");
        int64_t nine = 9;
        SturdyAsyncObject start{0};
        (void)sturdy_exec_just(constant_work, &nine, STURDY_LANE_LIGHT, &start);
        (void)sturdy_exec_then(start, then_fail_if_nine, nullptr, STURDY_LANE_BLOCKING, &refused);
        SturdyAsyncOp refusal{0};
        (void)sturdy_exec_sync_wait(refused, &refusal);
        check(status(refusal) == STURDY_OP_FAILED, "a failing stage fails the chain");
    }

    fs::remove_all(scratch);
    if (failures == 0) (void)std::printf("AsyncServeFfiTest passed\n");
    return failures == 0 ? 0 : 1;
}
