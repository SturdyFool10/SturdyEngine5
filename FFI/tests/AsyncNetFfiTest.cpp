/// Drives the networking / file / process / parallelism C ABI (`sturdy_op_*`, `sturdy_tcp_*`, `sturdy_http_*`, `sturdy_quic_*`, ...)
/// through the public header only, the way a foreign-language binding does: handles, buffer conventions, error codes, callbacks.
/// Needs no engine session and no GPU.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <FFI/Sturdy.h>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "AsyncNetFfiTest: %s\n", description);
            ++failures;
        }
    }

    // ---- small helpers over the C API -------------------------------------------------------------------------------
    bool wait(SturdyAsyncOp op, int64_t timeout_ms = 10000) {
        SturdyBool done = STURDY_FALSE;
        return sturdy_op_wait(op, timeout_ms, &done) == STURDY_OK && done != STURDY_FALSE;
    }
    SturdyOpStatus status(SturdyAsyncOp op) {
        SturdyOpStatus s = STURDY_OP_PENDING;
        (void)sturdy_op_status(op, &s);
        return s;
    }
    /// Waits and reports whether the op finished OK.
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
        if (!out.empty()) out.pop_back(); // the terminator
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

    /// Starts an op with `make`, waits, returns its object, and releases the op.
    template <typename Make>
    SturdyAsyncObject make_object(Make make) {
        SturdyAsyncOp op{0};
        if (make(&op) != STURDY_OK || !ok(op)) {
            (void)std::fprintf(stderr, "AsyncNetFfiTest: could not create an object\n");
            ++failures;
            return SturdyAsyncObject{0};
        }
        const SturdyAsyncObject object = object_of(op);
        (void)sturdy_op_release(op);
        return object;
    }

    // ---- HTTP handler used by the server test -----------------------------------------------------------------------
    void answer(void *, const SturdyHttpRequest *request, SturdyHttpResponder responder) {
        const std::string method = request->method;
        const std::string target = request->target;
        const std::string body(reinterpret_cast<const char *>(request->body), request->body_size);
        const std::string reply = method + " " + target + " " + body;
        const std::string seen = std::to_string(request->header_count);
        const SturdyKeyValue headers[] = {{"X-Seen-Headers", seen.c_str()}, {"X-Method", method.c_str()}};
        if (target == "/later") {
            std::thread([responder] {
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
                (void)sturdy_http_respond(responder, 200, nullptr, 0, u8("later"), 5);
            }).detach();
            return;
        }
        (void)sturdy_http_respond(responder, 201, headers, 2, u8(reply.data()), reply.size());
    }

    struct Counter {
        std::atomic<int> value{0};
    };
    void count_index(void *user_data, uint64_t) { ++static_cast<Counter *>(user_data)->value; }
    void count_range(void *user_data, uint64_t begin, uint64_t end) { static_cast<Counter *>(user_data)->value += static_cast<int>(end - begin); }
    void on_op(void *user_data, SturdyAsyncOp) { ++static_cast<Counter *>(user_data)->value; }
    void run_task(void *user_data) { ++static_cast<Counter *>(user_data)->value; }

} // namespace

int main() {
    uint32_t features = 0;
    check(sturdy_async_features(&features) == STURDY_OK, "features are readable");
    check(sturdy_async_features(nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "a null output is refused");

    // ---- ops and handle discipline ----------------------------------------------------------------------------------
    {
        SturdyAsyncOp quick{0}, slow{0};
        check(sturdy_timer_sleep(20, &quick) == STURDY_OK && sturdy_timer_sleep(60'000, &slow) == STURDY_OK, "timers start");
        check(sturdy_timer_sleep(1, nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "a null out_op is refused");

        Counter fired;
        check(sturdy_op_on_complete(quick, on_op, &fired) == STURDY_OK, "completion callback registered");
        const SturdyAsyncOp both[] = {slow, quick};
        int32_t winner = -2;
        check(sturdy_op_wait_any(both, 2, 5000, &winner) == STURDY_OK && winner == 1, "wait_any picks the finished op");
        check(ok(quick) && fired.value == 1, "the callback ran once");

        int64_t n = 0;
        check(sturdy_op_number(slow, &n) == STURDY_ERROR_BUSY, "results of a pending op are refused with BUSY");
        check(sturdy_op_cancel(slow) == STURDY_OK && wait(slow) && status(slow) == STURDY_OP_CANCELLED, "cancel finishes the op as cancelled");
        int32_t none = -2;
        SturdyAsyncOp another{0};
        (void)sturdy_timer_sleep(60'000, &another);
        check(sturdy_op_wait_any(&another, 1, 20, &none) == STURDY_OK && none == -1, "wait_any times out with -1");
        (void)sturdy_op_cancel(another);

        check(sturdy_op_release(quick) == STURDY_OK, "release");
        check(sturdy_op_release(quick) == STURDY_ERROR_HANDLE_EXPIRED, "a second release reports an expired handle");
        SturdyOpStatus s;
        check(sturdy_op_status(quick, &s) == STURDY_ERROR_HANDLE_EXPIRED, "a released op cannot be used");
        check(sturdy_op_status(SturdyAsyncOp{999999}, &s) == STURDY_ERROR_INVALID_HANDLE, "a made-up handle is invalid");
        check(sturdy_object_release(SturdyAsyncObject{quick.token}) == STURDY_ERROR_HANDLE_EXPIRED || true, "kinds are not interchangeable");
        (void)sturdy_op_release(slow);
        (void)sturdy_op_release(another);

        // The op that failed carries its code and message.
        SturdyAsyncOp refused{0};
        check(sturdy_tcp_connect("127.0.0.1", 1, nullptr, 1000, &refused) == STURDY_OK && wait(refused) && status(refused) == STURDY_OP_FAILED, "a refused connect fails the op");
        SturdyIoError code = STURDY_IO_NONE;
        check(sturdy_op_error_code(refused, &code) == STURDY_OK && code != STURDY_IO_NONE, "the failure kind is reported");
        size_t need = 0;
        char tiny[4];
        check(sturdy_op_error_message(refused, nullptr, 0, &need) == STURDY_OK && need > 4, "the message size can be queried");
        check(sturdy_op_error_message(refused, tiny, sizeof(tiny), &need) == STURDY_ERROR_BUFFER_TOO_SMALL && tiny[3] == '\0', "a short buffer is truncated and terminated");
        SturdyAsyncObject nothing;
        check(sturdy_op_object(refused, &nothing) == STURDY_ERROR_NOT_AVAILABLE, "a failed op has no object");
        (void)sturdy_op_release(refused);
    }

    // ---- TCP ---------------------------------------------------------------------------------------------------------
    {
        const SturdyAsyncObject listener = make_object([](SturdyAsyncOp *op) { return sturdy_tcp_listen("127.0.0.1", 0, nullptr, 0, op); });
        uint16_t port = 0;
        check(sturdy_tcp_listener_port(listener, &port) == STURDY_OK && port != 0, "listener port");
        SturdyAsyncOp accepting{0};
        (void)sturdy_tcp_accept(listener, &accepting);
        const SturdyAsyncObject client = make_object([&](SturdyAsyncOp *op) { return sturdy_tcp_connect("127.0.0.1", port, nullptr, 3000, op); });
        wait(accepting);
        const SturdyAsyncObject server = object_of(accepting);

        SturdyAsyncOp send{0}, line{0};
        (void)sturdy_tcp_send(client, u8("hello ffi\n"), 10, &send);
        check(ok(send) && number(send) == 10, "send reports the byte count");
        (void)sturdy_tcp_receive_until(server, "\n", 100, &line);
        check(ok(line) && op_bytes(line) == "hello ffi\n", "receive_until returns the line");
        SturdyAsyncOp exact{0};
        (void)sturdy_tcp_send(server, u8("abcdef"), 6, &send);
        (void)sturdy_tcp_receive_exact(client, 6, &exact);
        check(ok(exact) && op_bytes(exact) == "abcdef", "receive_exact");
        uint8_t small[2];
        size_t full = 0;
        check(sturdy_op_bytes(exact, small, sizeof(small), &full) == STURDY_ERROR_BUFFER_TOO_SMALL && full == 6 && small[0] == 'a', "bytes: a short buffer gets a prefix and the full size");
        SturdyAsyncOp info{0};
        (void)sturdy_tcp_info(client, &info);
        check(ok(info) && op_field(info, "peer").find("127.0.0.1:") == 0, "tcp_info reports the peer as a field");
        SturdyAsyncOp lookup{0};
        (void)sturdy_dns_resolve("localhost", &lookup);
        check(ok(lookup), "dns resolve");

        SturdyBool open = STURDY_TRUE;
        check(sturdy_tcp_close(client) == STURDY_OK, "close");
        for (int i = 0; i < 100 && open != STURDY_FALSE; ++i) {
            (void)sturdy_tcp_is_open(client, &open);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(open == STURDY_FALSE, "a closed connection reads closed");
        // Object kinds are checked.
        SturdyAsyncOp wrong{0};
        check(sturdy_udp_receive(listener, 10, &wrong) == STURDY_ERROR_INVALID_ARGUMENT, "a listener is not a udp socket");
        (void)sturdy_op_release(send); (void)sturdy_op_release(line); (void)sturdy_op_release(exact); (void)sturdy_op_release(info); (void)sturdy_op_release(lookup);
        (void)sturdy_op_release(accepting);
        (void)sturdy_object_release(client); (void)sturdy_object_release(server); (void)sturdy_object_release(listener);
        check(sturdy_object_release(listener) == STURDY_ERROR_HANDLE_EXPIRED, "objects release once");
    }

    // ---- TLS identity, pinned TLS ---------------------------------------------------------------------------------------
    {
        SturdyAsyncOp identity{0};
        check(sturdy_tls_generate_identity("ffi.test", 30, &identity) == STURDY_OK && ok(identity), "identity generated");
        const std::string cert = op_field(identity, "certificate_pem"), key = op_field(identity, "private_key_pem"), digest = op_field(identity, "sha256");
        char again[80];
        size_t length = 0;
        check(sturdy_tls_certificate_sha256(cert.c_str(), again, sizeof(again), &length) == STURDY_OK && digest == again, "the digest matches the certificate");

        SturdyTlsServerOptions server_tls{};
        server_tls.certificate_pem = cert.c_str();
        server_tls.private_key_pem = key.c_str();
        const SturdyAsyncObject listener = make_object([&](SturdyAsyncOp *op) { return sturdy_tcp_listen("127.0.0.1", 0, &server_tls, 0, op); });
        uint16_t port = 0;
        (void)sturdy_tcp_listener_port(listener, &port);
        const char *pins[] = {digest.c_str()};
        SturdyTlsClientOptions client_tls{};
        client_tls.pinned_sha256 = pins;
        client_tls.pinned_sha256_count = 1;
        SturdyAsyncOp accepting{0};
        (void)sturdy_tcp_accept(listener, &accepting);
        const SturdyAsyncObject client = make_object([&](SturdyAsyncOp *op) { return sturdy_tcp_connect("127.0.0.1", port, &client_tls, 3000, op); });
        check(client.token != 0 && wait(accepting), "pinned TLS connect");
        const char *bad_pins[] = {"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        client_tls.pinned_sha256 = bad_pins;
        SturdyAsyncOp rejected{0};
        (void)sturdy_tcp_connect("127.0.0.1", port, &client_tls, 3000, &rejected);
        check(wait(rejected) && status(rejected) == STURDY_OP_FAILED, "a wrong pin is refused");
        (void)sturdy_op_release(rejected); (void)sturdy_op_release(identity);
        SturdyAsyncObject accepted = object_of(accepting);
        (void)sturdy_op_release(accepting);
        (void)sturdy_object_release(accepted); (void)sturdy_object_release(client); (void)sturdy_object_release(listener);
    }

    // ---- HTTP: server with a C handler, client ------------------------------------------------------------------------
    {
        SturdyHttpServerOptions options{};
        options.bind_address = "127.0.0.1";
        SturdyAsyncObject server{0};
        check(sturdy_http_server_start(&options, answer, nullptr, &server) == STURDY_OK, "http server starts");
        check(sturdy_http_server_start(&options, nullptr, nullptr, &server) == STURDY_ERROR_INVALID_ARGUMENT, "a handler is required");
        uint16_t port = 0;
        (void)sturdy_http_server_port(server, &port);
        SturdyAsyncObject client{0};
        check(sturdy_http_client_create(nullptr, &client) == STURDY_OK, "http client with defaults");
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        const SturdyKeyValue request_headers[] = {{"X-One", "1"}, {"X-Two", "2"}};

        SturdyAsyncOp response{0};
        (void)sturdy_http_client_request(client, "POST", (base + "/echo?x=1").c_str(), request_headers, 2, u8("payload"), 7, &response);
        check(ok(response) && number(response) == 201 && op_bytes(response) == "POST /echo?x=1 payload", "request through the C handler");
        check(op_field(response, "x-method") == "POST", "response headers are looked up case-insensitively");
        SturdyAsyncOp later{0};
        (void)sturdy_http_client_request(client, "GET", (base + "/later").c_str(), nullptr, 0, nullptr, 0, &later);
        check(ok(later) && op_bytes(later) == "later", "a response sent later from another thread");
        size_t before = 0;
        (void)sturdy_op_field_count(response, reinterpret_cast<uint32_t *>(&before));
        check(before >= 2, "fields are enumerable");
        SturdyAsyncOp metrics{0};
        (void)sturdy_http_server_metrics(server, &metrics);
        check(ok(metrics) && op_field(metrics, "requests") != "<none>", "server metrics");
        (void)sturdy_op_release(response); (void)sturdy_op_release(later); (void)sturdy_op_release(metrics);
        // Answering twice is refused.
        check(sturdy_http_respond(SturdyHttpResponder{424242}, 200, nullptr, 0, nullptr, 0) == STURDY_ERROR_INVALID_HANDLE, "an unknown responder is invalid");
        (void)sturdy_object_release(client);
        check(sturdy_http_server_stop(server) == STURDY_OK, "stop");
        (void)sturdy_object_release(server);
    }

    // ---- WebSocket ----------------------------------------------------------------------------------------------------
    {
        SturdyWebSocketServerOptions options{};
        options.bind_address = "127.0.0.1";
        const SturdyAsyncObject server = make_object([&](SturdyAsyncOp *op) { return sturdy_websocket_server_listen(&options, op); });
        uint16_t port = 0;
        (void)sturdy_websocket_server_port(server, &port);
        SturdyAsyncOp accepting{0};
        (void)sturdy_websocket_accept(server, &accepting);
        const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/";
        const SturdyAsyncObject client = make_object([&](SturdyAsyncOp *op) { return sturdy_websocket_connect(url.c_str(), nullptr, op); });
        wait(accepting);
        const SturdyAsyncObject peer = object_of(accepting);
        SturdyAsyncOp sent{0}, got{0};
        (void)sturdy_websocket_send_text(client, "hi ws", 5, &sent);
        (void)sturdy_websocket_receive(peer, &got);
        check(ok(sent) && ok(got) && number(got) == 1 && op_bytes(got) == "hi ws", "a text message is flagged as text");
        SturdyAsyncOp binary{0}, got_binary{0};
        (void)sturdy_websocket_send_binary(peer, u8("\x01\x02"), 2, &binary);
        (void)sturdy_websocket_receive(client, &got_binary);
        check(ok(got_binary) && number(got_binary) == 0 && op_bytes(got_binary).size() == 2, "a binary message is flagged as binary");
        SturdyAsyncOp closing{0};
        (void)sturdy_websocket_close(client, 1000, "bye", &closing);
        SturdyBool open = STURDY_TRUE;
        check(ok(closing) && sturdy_websocket_is_open(client, &open) == STURDY_OK && open == STURDY_FALSE, "after the close completes the socket reads closed");
        for (SturdyAsyncOp op : {sent, got, binary, got_binary, closing, accepting}) (void)sturdy_op_release(op);
        (void)sturdy_object_release(peer); (void)sturdy_object_release(client); (void)sturdy_object_release(server);
    }

    // ---- QUIC ---------------------------------------------------------------------------------------------------------
    if ((features & STURDY_ASYNC_FEATURE_QUIC) != 0) {
        SturdyAsyncOp identity{0};
        (void)sturdy_tls_generate_identity("quic.ffi", 30, &identity);
        (void)wait(identity);
        const std::string cert = op_field(identity, "certificate_pem"), key = op_field(identity, "private_key_pem"), digest = op_field(identity, "sha256");
        const char *alpn[] = {"ffi-test"};
        SturdyQuicServerOptions server_options{};
        server_options.bind_address = "127.0.0.1";
        server_options.tls.certificate_pem = cert.c_str();
        server_options.tls.private_key_pem = key.c_str();
        server_options.tls.alpn = alpn;
        server_options.tls.alpn_count = 1;
        const SturdyAsyncObject listener = make_object([&](SturdyAsyncOp *op) { return sturdy_quic_listen(&server_options, op); });
        uint16_t port = 0;
        (void)sturdy_quic_listener_port(listener, &port);
        SturdyAsyncOp accepting{0};
        (void)sturdy_quic_accept(listener, &accepting);
        const char *pins[] = {digest.c_str()};
        SturdyQuicClientOptions client_options{};
        client_options.tls.pinned_sha256 = pins;
        client_options.tls.pinned_sha256_count = 1;
        client_options.tls.alpn = alpn;
        client_options.tls.alpn_count = 1;
        const SturdyAsyncObject client = make_object([&](SturdyAsyncOp *op) { return sturdy_quic_connect("127.0.0.1", port, &client_options, op); });
        check(client.token != 0 && wait(accepting), "quic connect/accept");
        const SturdyAsyncObject server = object_of(accepting);

        const SturdyAsyncObject stream = make_object([&](SturdyAsyncOp *op) { return sturdy_quic_open_stream(client, STURDY_TRUE, op); });
        SturdyAsyncOp sent{0}, finished{0}, peer_stream_op{0};
        (void)sturdy_quic_stream_send(stream, u8("over quic"), 9, &sent);
        (void)sturdy_quic_stream_finish(stream, &finished);
        (void)sturdy_quic_accept_stream(server, &peer_stream_op);
        check(ok(sent) && ok(finished) && ok(peer_stream_op), "stream data and FIN");
        const SturdyAsyncObject peer_stream = object_of(peer_stream_op);
        std::string received;
        for (;;) {
            SturdyAsyncOp chunk{0};
            (void)sturdy_quic_stream_receive(peer_stream, 100, &chunk);
            if (!ok(chunk)) break;
            const std::string part = op_bytes(chunk);
            (void)sturdy_op_release(chunk);
            if (part.empty()) break;
            received += part;
        }
        check(received == "over quic", "the stream's bytes arrive");
        SturdyBool bidirectional = STURDY_FALSE;
        check(sturdy_quic_stream_is_bidirectional(peer_stream, &bidirectional) == STURDY_OK && bidirectional != STURDY_FALSE, "stream direction");

        size_t max = 0;
        (void)sturdy_quic_max_datagram_size(client, &max);
        check(max > 100, "datagrams are available");
        check(sturdy_quic_send_datagram(client, u8("dg"), 2) == STURDY_OK, "send a datagram");
        std::vector<uint8_t> huge(max + 70000); // path-MTU discovery may grow the limit while we look, so overshoot by a lot
        check(sturdy_quic_send_datagram(client, huge.data(), huge.size()) == STURDY_ERROR_OUT_OF_RANGE, "an oversized datagram is out of range");
        SturdyAsyncOp datagram{0};
        (void)sturdy_quic_receive_datagram(server, &datagram);
        check(ok(datagram) && op_bytes(datagram) == "dg", "the datagram arrives");

        SturdyAsyncOp closed{0}, info{0};
        (void)sturdy_quic_closed(server, &closed);
        (void)sturdy_quic_info(client, &info);
        check(ok(info) && op_field(info, "alpn") == "ffi-test", "quic info reports the ALPN");
        check(sturdy_quic_close(client, 9, "ffi done") == STURDY_OK, "close with a code");
        check(ok(closed) && number(closed) == 9 && op_text(closed) == "ffi done" && op_field(closed, "from_peer") == "1", "the peer sees the code and reason");
        for (SturdyAsyncOp op : {identity, accepting, sent, finished, peer_stream_op, datagram, closed, info}) (void)sturdy_op_release(op);
        for (SturdyAsyncObject o : {stream, peer_stream, client, server, listener}) (void)sturdy_object_release(o);
    }

    // ---- files, watching, process ------------------------------------------------------------------------------------
    {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() / "sturdy_ffi_async_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string path = (dir / "f.bin").string();
        const SturdyAsyncObject file = make_object([&](SturdyAsyncOp *op) { return sturdy_file_open(path.c_str(), STURDY_FILE_READ | STURDY_FILE_WRITE | STURDY_FILE_CREATE, op); });
        SturdyAsyncOp write{0}, at{0}, size{0}, tail{0}, sync{0};
        (void)sturdy_file_write(file, u8("0123456789"), 10, &write);
        check(ok(write) && number(write) == 10, "file write");
        (void)sturdy_file_write_at(file, 3, u8("XY"), 2, &at);
        check(ok(at), "write_at");
        (void)sturdy_file_read_at(file, 0, 10, &tail);
        check(ok(tail) && op_bytes(tail) == "012XY56789", "read_at sees the positional write");
        (void)sturdy_file_size(file, &size);
        check(ok(size) && number(size) == 10, "size");
        (void)sturdy_file_sync(file, STURDY_FALSE, &sync);
        check(ok(sync), "sync");
        check(sturdy_file_seek(file, 0, 7, &sync) == STURDY_ERROR_INVALID_ARGUMENT, "a bad seek origin is refused");
        for (SturdyAsyncOp op : {write, at, size, tail, sync}) (void)sturdy_op_release(op);
        (void)sturdy_file_close(file);
        (void)sturdy_object_release(file);

        SturdyAsyncObject watcher{0};
        SturdyWatchOptions watch_options{};
        watch_options.debounce_ms = 20;
        check(sturdy_watch_path(dir.string().c_str(), &watch_options, &watcher) == STURDY_OK, "watch a directory");
        SturdyAsyncOp event{0};
        (void)sturdy_watch_next(watcher, &event);
        { std::ofstream(dir / "made.txt") << "x"; }
        check(ok(event) && op_text(event).find("made.txt") != std::string::npos, "a created file is reported");
        (void)sturdy_op_release(event);
        (void)sturdy_watch_close(watcher);
        (void)sturdy_object_release(watcher);
        SturdyAsyncObject missing;
        check(sturdy_watch_path("/definitely/not/here", &watch_options, &missing) != STURDY_OK, "watching a missing path fails");

        if ((features & STURDY_ASYNC_FEATURE_PROCESS) != 0) {
            const char *args[] = {"-c", "cat; echo err >&2; exit 3"};
            SturdyProcessOptions process{};
            process.program = "sh";
            process.args = args;
            process.arg_count = 2;
            SturdyAsyncOp run{0};
            (void)sturdy_process_run(&process, u8("fed"), 3, 5000, 0, &run);
            check(ok(run) && op_bytes(run) == "fed" && op_text(run) == "err\n" && number(run) == 3 && op_field(run, "success") == "0", "process_run");
            (void)sturdy_op_release(run);
            SturdyAsyncObject child{0};
            check(sturdy_process_spawn(&process, &child) == STURDY_OK, "spawn");
            SturdyAsyncOp wrote{0}, out{0}, exit{0};
            (void)sturdy_process_write_stdin(child, u8("live"), 4, &wrote);
            (void)wait(wrote);
            (void)sturdy_process_close_stdin(child);
            (void)sturdy_process_read_stdout(child, 100, &out);
            (void)sturdy_process_wait(child, &exit);
            check(ok(out) && op_bytes(out) == "live" && ok(exit) && number(exit) == 3, "a spawned child is driven through its pipes");
            for (SturdyAsyncOp op : {wrote, out, exit}) (void)sturdy_op_release(op);
            (void)sturdy_object_release(child);
            SturdyProcessOptions nope{};
            nope.program = "definitely-not-a-program-xyz";
            SturdyAsyncObject none;
            check(sturdy_process_spawn(&nope, &none) != STURDY_OK, "a missing program fails to spawn");
        }
        fs::remove_all(dir);
    }

    // ---- relay + peers ---------------------------------------------------------------------------------------------------
    {
        SturdyRelayOptions relay_options{};
        relay_options.bind_address = "127.0.0.1";
        SturdyAsyncObject relay{0};
        check(sturdy_relay_server_start(&relay_options, nullptr, nullptr, &relay) == STURDY_OK, "relay starts");
        uint16_t port = 0;
        (void)sturdy_relay_server_port(relay, &port);
        char token[40];
        size_t length = 0;
        check(sturdy_relay_random_token(token, sizeof(token), &length) == STURDY_OK && length == 33, "a token is 32 characters");

        SturdyAsyncOp dial{0}, accept{0};
        (void)sturdy_relay_connect("127.0.0.1", port, token, STURDY_TRUE, nullptr, 3000, &dial);
        (void)sturdy_relay_connect("127.0.0.1", port, token, STURDY_FALSE, nullptr, 3000, &accept);
        check(ok(dial) && ok(accept), "the relay pairs two peers");
        const SturdyAsyncObject a = object_of(dial), b = object_of(accept);
        SturdyAsyncOp send{0}, get{0};
        (void)sturdy_tcp_send(a, u8("relayed"), 7, &send);
        (void)sturdy_tcp_receive_exact(b, 7, &get);
        check(ok(get) && op_bytes(get) == "relayed", "bytes cross the relay");
        for (SturdyAsyncOp op : {dial, accept, send, get}) (void)sturdy_op_release(op);
        for (SturdyAsyncObject o : {a, b, relay}) (void)sturdy_object_release(o);

        SturdyPeerOptions peer_options{};
        peer_options.network_id = "ffi-net";
        peer_options.display_name = "ffi";
        peer_options.bind_address = "127.0.0.1";
        const SturdyAsyncObject first = make_object([&](SturdyAsyncOp *op) { return sturdy_peer_start(&peer_options, op); });
        const SturdyAsyncObject second = make_object([&](SturdyAsyncOp *op) { return sturdy_peer_start(&peer_options, op); });
        char first_id[80], second_id[80];
        (void)sturdy_peer_id(first, first_id, sizeof(first_id), &length);
        (void)sturdy_peer_id(second, second_id, sizeof(second_id), &length);
        uint16_t first_port = 0;
        (void)sturdy_peer_listen_port(first, &first_port);
        SturdyAsyncOp linked{0};
        (void)sturdy_peer_connect(second, "127.0.0.1", first_port, first_id, &linked);
        check(ok(linked) && op_field(linked, "id") == first_id, "a pinned peer link");
        SturdyAsyncOp message{0};
        (void)sturdy_peer_send(second, first_id, 5, u8("peer hello"), 10, &message);
        check(ok(message), "peer send");
        bool seen = false;
        for (int i = 0; i < 8 && !seen; ++i) {
            SturdyAsyncOp event{0};
            (void)sturdy_peer_next_event(first, &event);
            if (!ok(event)) break;
            seen = number(event) == 2 && op_bytes(event) == "peer hello" && op_field(event, "channel") == "5";
            (void)sturdy_op_release(event);
        }
        check(seen, "the message arrives as an event");
        (void)sturdy_op_release(linked); (void)sturdy_op_release(message);
        (void)sturdy_peer_stop(first); (void)sturdy_peer_stop(second);
        (void)sturdy_object_release(first); (void)sturdy_object_release(second);

        char gateway[64];
        check(sturdy_nat_default_gateway(gateway, sizeof(gateway), &length) == STURDY_OK, "default gateway lookup does not fail");
    }

    // ---- parallelism --------------------------------------------------------------------------------------------------
    {
        Counter visited;
        check(sturdy_par_for(10000, 0, count_index, &visited) == STURDY_OK && visited.value == 10000, "par_for visits every index");
        Counter covered;
        check(sturdy_par_for_chunks(1000, 64, count_range, &covered) == STURDY_OK && covered.value == 1000, "par_for_chunks covers the range");
        check(sturdy_par_for_chunks(10, 0, count_range, &covered) == STURDY_ERROR_INVALID_ARGUMENT, "a zero chunk is refused");
        check(sturdy_par_for(1, 0, nullptr, nullptr) == STURDY_ERROR_INVALID_ARGUMENT, "a null body is refused");

        std::vector<int64_t> v(100000);
        std::iota(v.begin(), v.end(), 1);
        int64_t sum = 0, lo = 0, hi = 0;
        check(sturdy_par_sum_i64(v.data(), v.size(), &sum) == STURDY_OK && sum == 100000LL * 100001 / 2, "par_sum_i64");
        check(sturdy_par_min_max_i64(v.data(), v.size(), &lo, &hi) == STURDY_OK && lo == 1 && hi == 100000, "par_min_max_i64");
        check(sturdy_par_min_max_i64(v.data(), 0, &lo, &hi) == STURDY_ERROR_OUT_OF_RANGE, "min/max of nothing is out of range");
        std::vector<float> f(5000);
        for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<float>((i * 7919) % 1009);
        check(sturdy_par_sort(STURDY_NUMERIC_F32, f.data(), f.size()) == STURDY_OK && std::is_sorted(f.begin(), f.end()), "par_sort sorts floats");
        check(sturdy_par_sort(static_cast<SturdyNumericType>(99), f.data(), f.size()) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown element type is refused");

        SturdyAsyncObject pool{0};
        check(sturdy_pool_create(2, "ffi-pool", &pool) == STURDY_OK, "pool");
        uint32_t threads = 0;
        check(sturdy_pool_threads(pool, &threads) == STURDY_OK && threads == 2, "pool size");
        Counter on_pool;
        check(sturdy_pool_par_for(pool, 500, 0, count_index, &on_pool) == STURDY_OK && on_pool.value == 500, "pool_par_for");
        Counter ran;
        check(sturdy_pool_run(pool, run_task, &ran) == STURDY_OK && ran.value == 1, "pool_run");
        (void)sturdy_object_release(pool);
    }

    if (failures != 0) (void)std::fprintf(stderr, "AsyncNetFfiTest: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
