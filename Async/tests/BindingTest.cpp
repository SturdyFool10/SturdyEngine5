// The flat binding facade (what the C ABI and the Rust shim sit on) driven end to end: ops, objects, every protocol family.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
namespace B = SFT::Async::Binding;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("BindingTest FAILED: {}", what);
            ++failures;
        }
    }

    B::Bytes bytes(std::string_view text) { return B::Bytes(text.begin(), text.end()); }
    std::string text(const B::Bytes &data) { return std::string(data.begin(), data.end()); }

    /// Waits for `op` (10 s cap) and returns its value.
    B::Value finish(const B::OpPtr &op) {
        if (!op->wait(10'000)) {
            Foundation::log_error("BindingTest: an operation did not finish");
            std::abort();
        }
        return op->result();
    }

    bool ok(const B::OpPtr &op) { return finish(op).status == B::OpStatus::Ok; }

} // namespace

int main() {
    Scheduler::initialize(2);

    // ---- ops: polling, callbacks, cancellation, wait_any ----
    {
        auto quick = B::sleep(20);
        auto slow = B::sleep(5000);
        check(!quick->done(), "an op starts pending");
        std::atomic<int> called{0};
        quick->on_complete([&] { ++called; });
        check(B::wait_any({slow, quick}, 3000) == 1, "wait_any returns the first op to finish");
        check(called == 1 && quick->status() == B::OpStatus::Ok, "the completion callback ran once");
        quick->on_complete([&] { ++called; });
        check(called == 2, "a callback registered after completion runs immediately");
        slow->cancel();
        check(finish(slow).status == B::OpStatus::Cancelled, "cancelling a sleep finishes it as Cancelled");
        check(B::wait_any({B::sleep(5000)}, 30) == -1, "wait_any times out with -1");
    }

    // ---- misuse is reported, not undefined ----
    {
        bool threw = false;
        try {
            B::tcp_close(B::ObjectPtr{});
        } catch (const B::BindingError &error) {
            threw = error.code() == IoErrorCode::InvalidArgument;
        }
        check(threw, "a null object is an InvalidArgument error");
        auto udp = finish(B::udp_bind("127.0.0.1", 0, {})).object;
        threw = false;
        try {
            (void)B::tcp_is_open(udp);
        } catch (const B::BindingError &) {
            threw = true;
        }
        check(threw, "an object of the wrong kind is rejected");
        auto refused = B::tcp_connect("127.0.0.1", 1, nullptr, 1000);
        check(finish(refused).status == B::OpStatus::Failed && !refused->result().error.empty(), "a failed connect carries a code and a message");
    }

    // ---- TCP echo, UDP, DNS ----
    {
        auto listener = finish(B::tcp_listen("127.0.0.1", 0, nullptr, 0)).object;
        const auto port = B::tcp_listener_port(listener);
        auto accepting = B::tcp_accept(listener);
        auto client = finish(B::tcp_connect("127.0.0.1", port, nullptr, 3000)).object;
        auto server = finish(accepting).object;
        check(client && server, "tcp connect/accept");
        check(ok(B::tcp_send(client, bytes("hello binding\n"))), "tcp send");
        check(text(finish(B::tcp_receive_until(server, "\n", 100)).bytes) == "hello binding\n", "receive_until");
        check(ok(B::tcp_send(server, bytes("0123456789"))), "send back");
        check(text(finish(B::tcp_receive_exact(client, 10)).bytes) == "0123456789", "receive_exact");
        check(ok(B::tcp_shutdown_write(client)), "half close");
        check(finish(B::tcp_receive(server, 10)).bytes.empty(), "end of stream reads as empty");
        const auto info = B::tcp_info(client);
        check(!info.empty() && info.front().first == "peer", "tcp_info reports the peer");
        B::tcp_close(client);
        for (int i = 0; i < 100 && B::tcp_is_open(client); ++i) std::this_thread::sleep_for(10ms);
        check(!B::tcp_is_open(client), "closed");

        auto a = finish(B::udp_bind("127.0.0.1", 0, {})).object;
        auto b = finish(B::udp_bind("127.0.0.1", 0, {})).object;
        check(ok(B::udp_send_to(a, "127.0.0.1", B::udp_port(b), bytes("ping"))), "udp send");
        const auto &got = finish(B::udp_receive(b, 100));
        check(text(got.bytes) == "ping" && got.number == B::udp_port(a), "udp receive reports the sender");
        check(!finish(B::resolve_host("localhost")).fields.empty(), "resolve_host");
    }

    // ---- TLS identity + pinned TLS server ----
    {
        const auto identity = B::generate_identity("binding.test", 30);
        check(identity.sha256 == B::certificate_sha256(identity.certificate_pem), "identity digest");
        B::TlsServerSpec server_tls;
        server_tls.certificate_pem = identity.certificate_pem;
        server_tls.private_key_pem = identity.private_key_pem;
        auto listener = finish(B::tcp_listen("127.0.0.1", 0, &server_tls, 0)).object;
        B::TlsClientSpec client_tls;
        client_tls.pinned_sha256 = {identity.sha256};
        auto accepting = B::tcp_accept(listener);
        auto client = finish(B::tcp_connect("127.0.0.1", B::tcp_listener_port(listener), &client_tls, 3000)).object;
        auto server = finish(accepting).object;
        check(client && server, "pinned TLS connect");
        (void)finish(B::tcp_send(client, bytes("secret")));
        check(text(finish(B::tcp_receive_exact(server, 6)).bytes) == "secret", "data over TLS");
        bool has_protocol = false;
        for (const auto &[k, v] : B::tcp_info(client)) has_protocol = has_protocol || (k == "protocol" && v.rfind("TLS", 0) == 0);
        check(has_protocol, "tcp_info reports the TLS protocol");
        B::TlsClientSpec wrong;
        wrong.pinned_sha256 = {std::string(64, 'f')};
        check(finish(B::tcp_connect("127.0.0.1", B::tcp_listener_port(listener), &wrong, 3000)).status == B::OpStatus::Failed, "a wrong pin is refused");
    }

    // ---- HTTP server + client (+ HTTP/1.1 ping-pong with headers and bodies) ----
    {
        B::HttpServerSpec spec;
        spec.bind_address = "127.0.0.1";
        auto server = B::http_server_start(spec, [](B::HttpRequestView request, std::function<void(B::HttpResponseSpec)> respond) {
            B::HttpResponseSpec response;
            if (request.target == "/later") {
                std::thread([respond = std::move(respond)] {
                    std::this_thread::sleep_for(50ms);
                    B::HttpResponseSpec late;
                    late.body = bytes("answered later");
                    respond(std::move(late));
                }).detach();
                return;
            }
            response.status = 201;
            response.headers.push_back({"X-Echo-Method", request.method});
            response.body = bytes(request.method + " " + request.target + " " + text(request.body));
            respond(std::move(response));
        });
        const auto port = B::http_server_port(server);
        auto client = B::http_client_new({});
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        const auto &r = finish(B::http_request(client, "POST", base + "/echo?x=1", {{"X-Test", "1"}}, bytes("payload")));
        check(r.status == B::OpStatus::Ok && r.number == 201 && text(r.bytes) == "POST /echo?x=1 payload", "http request/response through the handler");
        bool echoed = false;
        for (const auto &[k, v] : r.fields) echoed = echoed || (k == "X-Echo-Method" && v == "POST");
        check(echoed, "response headers come back as fields");
        check(text(finish(B::http_request(client, "GET", base + "/later", {}, {})).bytes) == "answered later", "a handler may answer later from another thread");
        std::vector<B::OpPtr> many;
        for (int i = 0; i < 20; ++i) many.push_back(B::http_request(client, "GET", base + "/n" + std::to_string(i), {}, {}));
        int good = 0;
        for (auto &op : many) good += finish(op).number == 201;
        check(good == 20, "20 concurrent requests");
        check(B::http_server_http3_port(server) == 0, "no HTTP/3 unless asked for");
        bool counted = false;
        for (const auto &[k, v] : B::http_server_metrics(server)) counted = counted || (k == "requests" && std::stoi(v) >= 22);
        check(counted, "server metrics count the requests");
        B::http_server_stop(server);
    }

    // ---- WebSocket ----
    {
        B::WebSocketServerSpec spec;
        spec.bind_address = "127.0.0.1";
        spec.subprotocols = {"bind.v1"};
        auto server = finish(B::websocket_server_listen(spec)).object;
        auto accepting = B::websocket_accept(server);
        B::WebSocketClientSpec client_spec;
        client_spec.subprotocols = {"bind.v1"};
        auto client = finish(B::websocket_connect("ws://127.0.0.1:" + std::to_string(B::websocket_server_port(server)) + "/chat", client_spec)).object;
        auto peer = finish(accepting).object;
        check(client && peer, "websocket connect/accept");
        check(ok(B::websocket_send_text(client, "hi there")), "send_text");
        const auto &m = finish(B::websocket_receive(peer));
        check(m.number == 1 && text(m.bytes) == "hi there", "text message arrives flagged as text");
        check(ok(B::websocket_send_binary(peer, bytes("bin"))), "send_binary");
        const auto &b = finish(B::websocket_receive(client));
        check(b.number == 0 && text(b.bytes) == "bin", "binary message arrives flagged as binary");
        bool negotiated = false;
        for (const auto &[k, v] : B::websocket_info(client)) negotiated = negotiated || (k == "subprotocol" && v == "bind.v1");
        check(negotiated, "the subprotocol was negotiated");
        check(ok(B::websocket_close(client, 1000, "bye")) && !B::websocket_is_open(client), "close completes and the socket reads closed");
    }

    // ---- QUIC ----
    if (B::features() & B::FeatureQuic) {
        const auto identity = B::generate_identity("quic.test", 30);
        B::QuicServerSpec server_spec;
        server_spec.bind_address = "127.0.0.1";
        server_spec.tls.certificate_pem = identity.certificate_pem;
        server_spec.tls.private_key_pem = identity.private_key_pem;
        server_spec.tls.alpn = {"bind-test"};
        auto listener = finish(B::quic_listen(server_spec)).object;
        auto accepting = B::quic_accept(listener);
        B::QuicClientSpec client_spec;
        client_spec.tls.pinned_sha256 = {identity.sha256};
        client_spec.tls.alpn = {"bind-test"};
        auto client = finish(B::quic_connect("127.0.0.1", B::quic_listener_port(listener), client_spec)).object;
        auto server = finish(accepting).object;
        check(client && server, "quic connect/accept");
        auto opening = B::quic_open_stream(client, true);
        auto stream = finish(opening).object;
        (void)finish(B::quic_stream_send(stream, bytes("over quic")));
        (void)finish(B::quic_stream_finish(stream));
        auto peer_stream = finish(B::quic_accept_stream(server)).object;
        std::string received;
        for (;;) {
            const auto &chunk = finish(B::quic_stream_receive(peer_stream, 100));
            if (chunk.bytes.empty()) break;
            received += text(chunk.bytes);
        }
        check(received == "over quic" && B::quic_stream_is_bidirectional(peer_stream), "a stream carries data to its end");
        B::quic_send_datagram(client, bytes("dgram"));
        check(text(finish(B::quic_receive_datagram(server)).bytes) == "dgram", "a datagram arrives");
        check(B::quic_max_datagram_size(client) > 100, "max datagram size is reported");
        bool tls_known = false;
        for (const auto &[k, v] : B::quic_info(client)) tls_known = tls_known || (k == "alpn" && v == "bind-test");
        check(tls_known, "quic_info reports the ALPN");
        auto closed = B::quic_closed(server);
        B::quic_close(client, 5, "done");
        const auto &info = finish(closed);
        check(info.number == 5 && info.text == "done", "the close code reaches the peer");
    }

    // ---- files, process, watching ----
    {
        const fs::path dir = fs::temp_directory_path() / "sturdy_binding_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string path = (dir / "f.bin").string();
        auto file = finish(B::file_open(path, B::FileRead | B::FileWrite | B::FileCreate)).object;
        check(file != nullptr, "file_open");
        (void)finish(B::file_write(file, bytes("0123456789")));
        (void)finish(B::file_write_at(file, 2, bytes("XY")));
        check(text(finish(B::file_read_at(file, 0, 10)).bytes) == "01XY456789", "write_at/read_at");
        check(finish(B::file_size(file)).number == 10, "file_size");
        (void)finish(B::file_seek(file, 0, 0));
        check(text(finish(B::file_read_to_end(file, 100)).bytes) == "01XY456789", "read_to_end");
        check(ok(B::file_sync(file, false)), "sync");
        B::file_close(file);

        auto watcher = B::watch_path(dir.string(), B::WatchSpec{false, 20, false, 250, 4096});
        auto next = B::watch_next(watcher);
        { std::ofstream(dir / "new.txt") << "x"; }
        const auto &event = finish(next);
        check(event.status == B::OpStatus::Ok && event.text.find("new.txt") != std::string::npos, "a created file produces a watch event");
        B::watch_close(watcher);

        if (B::features() & B::FeatureProcess) {
            B::ProcessSpec spec;
            spec.program = "sh";
            spec.args = {"-c", "cat; echo err >&2; exit 3"};
            const auto &run = finish(B::process_run(spec, bytes("fed"), 5000, 0));
            if (run.status != B::OpStatus::Ok) Foundation::log_info("process_run: {}", run.error);
            check(run.status == B::OpStatus::Ok && text(run.bytes) == "fed" && run.text == "err\n" && run.number == 3, "process_run feeds stdin and collects both outputs");
            auto child = B::process_spawn(spec);
            (void)finish(B::process_write_stdin(child, bytes("live")));
            B::process_close_stdin(child);
            check(text(finish(B::process_read_stdout(child, 100)).bytes) == "live", "a spawned child can be driven");
            check(finish(B::process_wait(child)).number == 3, "process_wait reports the exit code");
        }
        fs::remove_all(dir);
    }

    // ---- relay + NAT bits ----
    {
        B::RelaySpec spec;
        spec.bind_address = "127.0.0.1";
        auto relay = B::relay_server_start(spec, {});
        const auto token = B::random_relay_token();
        check(token.size() == 32, "relay token");
        auto dial = B::relay_connect("127.0.0.1", B::relay_server_port(relay), token, true, nullptr, 3000);
        auto accept = B::relay_connect("127.0.0.1", B::relay_server_port(relay), token, false, nullptr, 3000);
        auto a = finish(dial).object;
        auto b = finish(accept).object;
        check(a && b, "relay pairs two peers");
        (void)finish(B::tcp_send(a, bytes("via relay")));
        check(text(finish(B::tcp_receive_exact(b, 9)).bytes) == "via relay", "bytes cross the relay");
        B::relay_server_stop(relay);
        (void)B::default_gateway();
    }

    // ---- parallel ----
    {
        std::vector<std::int64_t> v(100000);
        std::iota(v.begin(), v.end(), 1);
        std::atomic<std::int64_t> total{0};
        B::par_for(v.size(), 1, [&](std::uint64_t i) { total += v[i]; });
        check(total == 100000LL * 100001 / 2, "par_for visits every index once");
        std::atomic<std::uint64_t> covered{0};
        B::par_for_chunks(1000, 64, [&](std::uint64_t begin, std::uint64_t end) { covered += end - begin; });
        check(covered == 1000, "par_for_chunks covers the range exactly");
        check(B::par_sum_i64(v.data(), v.size()) == 100000LL * 100001 / 2, "par_sum_i64");
        std::vector<double> d(50000, 0.5);
        check(B::par_sum_f64(d.data(), d.size()) == 25000.0, "par_sum_f64");
        std::int64_t lo = 0, hi = 0;
        check(B::par_min_max_i64(v.data(), v.size(), &lo, &hi) && lo == 1 && hi == 100000, "par_min_max_i64");
        check(!B::par_min_max_i64(v.data(), 0, &lo, &hi), "min/max of nothing is reported");
        std::vector<float> f(20000);
        for (std::size_t i = 0; i < f.size(); ++i) f[i] = static_cast<float>((i * 7919) % 10007);
        B::par_sort(B::Numeric::F32, f.data(), f.size());
        check(std::is_sorted(f.begin(), f.end()), "par_sort sorts floats");
        auto pool = B::pool_create(2, "binding-pool");
        check(B::pool_threads(pool) == 2, "pool size");
        std::atomic<int> on_pool{0};
        B::pool_par_for(pool, 1000, 1, [&](std::uint64_t) { ++on_pool; });
        check(on_pool == 1000, "pool_par_for runs the body for every index");
        bool threw = false;
        try {
            B::par_for(10, 1, [](std::uint64_t i) {
                if (i == 3) throw std::runtime_error("boom");
            });
        } catch (const std::runtime_error &) {
            threw = true;
        }
        check(threw, "an exception in a parallel body propagates to the caller");
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
