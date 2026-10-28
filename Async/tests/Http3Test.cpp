// HTTP/3 over QUIC: our server driven by curl (interop), Alt-Svc advertisement, and HttpClient speaking h3.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("Http3Test FAILED: {}", what);
            ++failures;
        }
    }

    struct CurlResult {
        int code = -1;
        std::string out;
    };

    CurlResult curl(std::vector<std::string> args, std::vector<std::byte> stdin_data = {}) {
        ProcessOptions options;
        options.program = "curl";
        options.args = std::move(args);
        RunLimits limits;
        limits.timeout = 25s;
        limits.max_output_bytes = 64 * 1024 * 1024;
        auto result = run_process(options, std::move(stdin_data), limits).wait();
        if (!result) return {-1, result.error().message};
        return {result->status.code, result->stdout_text()};
    }

    std::string write_file(const fs::path &path, const std::string &content) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
        return path.string();
    }

    std::vector<std::byte> to_bytes(const std::string &text) {
        std::vector<std::byte> v(text.size());
        std::memcpy(v.data(), text.data(), text.size());
        return v;
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    if (!quic_available()) {
        Foundation::log_info("Http3Test: QUIC not built in; skipped");
        Scheduler::shutdown();
        return 0;
    }
    const fs::path tests_dir = fs::path(__FILE__).parent_path();
    const fs::path root = fs::temp_directory_path() / "sturdy_h3_test";
    fs::remove_all(root);
    std::string big(3 * 1024 * 1024, 'z');
    for (std::size_t i = 0; i < big.size(); i += 997) big[i] = static_cast<char>('a' + (i / 997) % 26);
    write_file(root / "big.bin", big);
    write_file(root / "small.txt", "0123456789");

    Router router;
    router.get("/hello", [](const RouteRequest &) { return text_response(200, "hello over h3"); });
    router.post("/echo", [](const RouteRequest &r) { return text_response(200, r.http.body); });
    router.get("/inspect", [](const RouteRequest &r) {
        return text_response(200, "host=" + std::string(r.header("host").value_or("-")) + " ua=" + std::string(r.header("user-agent").value_or("-")) +
                                      " cookie=" + std::string(r.header("cookie").value_or("-")) + " ver=" + std::to_string(r.http.version) +
                                      " peer=" + r.http.remote_address);
    });
    router.get("/big", [&](const RouteRequest &) { return text_response(200, big); });
    router.get("/slow", [](const RouteRequest &) {
        return stream_response(stream_from([n = 0]() mutable -> TaskHandle<std::optional<std::string>> {
                                   if (n >= 4) return ready(std::optional<std::string>{});
                                   const std::string part = "chunk" + std::to_string(n++) + ";";
                                   return then(sleep_for(25ms), [part] { return std::optional<std::string>(part); });
                               }),
                               "text/plain");
    });
    router.route_async("GET", "/delayed", [](const RouteRequest &r) {
        const int ms = std::stoi(r.query_param("ms").value_or("50"));
        return then(sleep_for(std::chrono::milliseconds(ms)), [ms] { return text_response(200, "waited " + std::to_string(ms)); });
    });
    router.get("/inject", [](const RouteRequest &) {
        HttpResponseMessage response = text_response(200, "x");
        response.headers.push_back({"X-Evil", "a\r\nSet-Cookie: pwned=1"});
        return response;
    });
    router.get("/cookie", [](const RouteRequest &) {
        HttpResponseMessage response = text_response(200, "cookie set");
        response.headers.push_back({"Set-Cookie", "sid=h3session; Path=/"});
        return response;
    });
    StaticFilesOptions statics;
    statics.root = root.string();
    router.static_files("/files", statics);

    HttpServerOptions options;
    options.bind_address = "127.0.0.1";
    options.tls_certificate_chain_file = (tests_dir / "test-server-cert.pem").string();
    options.tls_private_key_file = (tests_dir / "test-server-key.pem").string();
    options.max_request_body_bytes = 256 * 1024;
    options.enable_http3 = true;
    HttpServer server(options, router.handler());
    auto started = server.start();
    check(started.has_value(), "TLS server with HTTP/3 starts");
    if (!started) {
        Foundation::log_error("  {}", started.error().message);
        Scheduler::shutdown();
        return 1;
    }
    const auto tcp_port = server.local_port().value();
    const auto udp_port = server.http3_local_port();
    check(udp_port && *udp_port == tcp_port, "HTTP/3 listens on the UDP port with the TCP port's number");
    const std::string base = "https://127.0.0.1:" + std::to_string(tcp_port);

    auto h3 = [&](std::string path, std::vector<std::string> extra = {}) {
        std::vector<std::string> args = {"-sk", "--http3-only", "--max-time", "15", "-w", "\n%{http_version}|%{http_code}"};
        args.insert(args.end(), extra.begin(), extra.end());
        args.push_back(base + path);
        return curl(std::move(args));
    };
    auto trailer = [](const std::string &out) { return out.substr(out.rfind('\n') + 1); };
    auto body_of = [](const std::string &out) { return out.substr(0, out.rfind('\n')); };

    if (std::system("curl --version | grep -q HTTP3") != 0) {
        Foundation::log_info("Http3Test: this curl has no HTTP/3; interop checks skipped");
    } else {
        {
            auto r = h3("/hello");
            check(r.code == 0 && body_of(r.out) == "hello over h3" && trailer(r.out) == "3|200", "GET over HTTP/3");
        }
        {
            auto r = h3("/inspect", {"-H", "User-Agent: probe/1", "-H", "Cookie: a=1", "-H", "Cookie: b=2"});
            const auto body = body_of(r.out);
            check(body.find("host=127.0.0.1:") == 0 && body.find("ua=probe/1") != std::string::npos && body.find("ver=30") != std::string::npos &&
                      body.find("peer=127.0.0.1") != std::string::npos,
                  "headers map as for HTTP/1 handlers, version reports 3.0, the peer address is the client's");
        }
        {
            const std::string payload(100 * 1024, 'q');
            auto echoed = curl({"-sk", "--http3-only", "-X", "POST", "--data-binary", "@-", "-w", "\n%{http_version}", base + "/echo"}, to_bytes(payload));
            check(body_of(echoed.out) == payload && trailer(echoed.out) == "3", "a 100 KiB request body round-trips");
            auto rejected = curl({"-sk", "--http3-only", "-X", "POST", "--data-binary", "@-", "-w", "\n%{http_code}", base + "/echo"}, to_bytes(std::string(400 * 1024, 'x')));
            check(trailer(rejected.out) == "413" || rejected.code != 0, "a request body over the limit is refused (413 or a stream reset)");
        }
        {
            auto r = h3("/big");
            check(r.code == 0 && body_of(r.out) == big && trailer(r.out) == "3|200", "a 3 MiB response crosses QUIC flow control intact");
            auto slow = h3("/slow");
            check(body_of(slow.out) == "chunk0;chunk1;chunk2;chunk3;", "a streamed (producer) response works over HTTP/3");
            auto file = h3("/files/big.bin");
            check(body_of(file.out) == big, "a file response is read on the IO lane and streamed over HTTP/3");
            auto range = h3("/files/small.txt", {"-H", "Range: bytes=2-5"});
            check(body_of(range.out) == "2345" && trailer(range.out) == "3|206", "byte ranges work over HTTP/3");
            auto head = curl({"-skI", "--http3-only", "-w", "%{http_version}", base + "/files/small.txt"});
            check(head.out.find("content-length: 10") != std::string::npos && head.out.rfind("3") == head.out.size() - 1, "HEAD reports content-length without a body");
        }
        {
            std::vector<std::string> args = {"-sk", "--http3-only", "--parallel", "--parallel-max", "20", "-w", "%{http_code},%{http_version}\n"};
            for (int i = 0; i < 20; ++i) {
                args.push_back("-o");
                args.push_back("/dev/null");
                args.push_back(base + "/delayed?ms=150");
            }
            const auto start = std::chrono::steady_clock::now();
            auto r = curl(args);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            int ok = 0;
            for (std::size_t at = 0; at < r.out.size();) {
                auto end = r.out.find('\n', at);
                if (end == std::string::npos) break;
                if (r.out.compare(at, end - at, "200,3") == 0) ++ok;
                at = end + 1;
            }
            check(r.code == 0 && ok == 20, "20 parallel HTTP/3 requests all succeed");
            check(elapsed < 2500ms, "...concurrently (20 x 150 ms handlers in well under 3 s)");
        }
        {
            auto injected = h3("/inject");
            check(trailer(injected.out) == "3|500" && body_of(injected.out).find("pwned") == std::string::npos, "a response header with CR/LF is refused on HTTP/3 too");
        }
        {
            auto r = curl({"-sk", "--http1.1", "-D", "-", "-o", "/dev/null", base + "/hello"});
            check(r.out.find("alt-svc: h3=\":" + std::to_string(tcp_port) + "\"") != std::string::npos ||
                      r.out.find("Alt-Svc: h3=\":" + std::to_string(tcp_port) + "\"") != std::string::npos,
                  "HTTP/1.1 responses advertise HTTP/3 with Alt-Svc");
            auto r2 = curl({"-sk", "--http2", "-D", "-", "-o", "/dev/null", base + "/hello"});
            check(r2.out.find("h3=\":" + std::to_string(tcp_port) + "\"") != std::string::npos, "...and so do HTTP/2 responses");
        }
    }

    // ---- the engine as a client: HttpClient learns Alt-Svc, switches to h3, multiplexes ----
    {
        auto cert_pem = read_text_file((tests_dir / "test-server-cert.pem").string());
        HttpClientOptions client_options;
        if (cert_pem) client_options.tls.pinned_sha256 = {certificate_sha256(*cert_pem).value()};
        client_options.cookies = true;
        HttpClient client(client_options);
        auto via = [](const std::expected<HttpResponseMessage, IoError> &r) { return r ? std::string(header_value(r->headers, "server").value_or("")) : std::string("error"); };

        auto first = client.get(base + "/hello").wait();
        check(first && first->body == "hello over h3" && via(first).find("HTTP/2") != std::string::npos, "the first request goes over TCP (h2) and sees the Alt-Svc");
        auto second = client.get(base + "/hello").wait();
        check(second && second->body == "hello over h3" && via(second).find("HTTP/3") != std::string::npos, "the next request goes over HTTP/3");

        const auto start = std::chrono::steady_clock::now();
        std::vector<TaskHandle<std::expected<HttpResponseMessage, IoError>>> inflight;
        for (int i = 0; i < 30; ++i) inflight.push_back(client.get(base + "/delayed?ms=200"));
        int ok = 0;
        for (auto &h : inflight) {
            auto r = h.wait();
            if (r && r->body == "waited 200" && via(r).find("HTTP/3") != std::string::npos) ++ok;
        }
        check(ok == 30, "30 concurrent requests all succeed over HTTP/3");
        check(std::chrono::steady_clock::now() - start < 3s, "...multiplexed (30 x 200 ms in well under 6 s)");

        auto big_response = client.get(base + "/big").wait();
        check(big_response && big_response->body == big, "a 3 MiB response through HttpClient over h3");
        auto streamed = client.get(base + "/slow").wait();
        check(streamed && streamed->body == "chunk0;chunk1;chunk2;chunk3;", "a streamed response through HttpClient over h3");
        const std::string upload(150 * 1024, 'u');
        auto echoed = client.post(base + "/echo", upload).wait();
        check(echoed && echoed->body == upload && via(echoed).find("HTTP/3") != std::string::npos, "a request body goes up over h3");
        (void)client.get(base + "/cookie").wait();
        auto inspect = client.get(base + "/inspect").wait();
        check(inspect && inspect->body.find("cookie=sid=h3session") != std::string::npos && inspect->body.find("ver=30") != std::string::npos, "cookies work over h3");
        auto file = client.get(base + "/files/small.txt", {{"Range", "bytes=2-5"}}).wait();
        check(file && file->status == 206 && file->body == "2345", "ranges work over h3");

        HttpClientOptions tiny = client_options;
        tiny.max_response_body_bytes = 64 * 1024;
        tiny.http3_prior_knowledge = true;
        HttpClient limited(tiny);
        check(!limited.get(base + "/big").wait().has_value(), "the response size limit applies to HTTP/3 bodies");

        CancellationSource cancel;
        auto pending = client.get(base + "/delayed?ms=3000", {}, cancel.token());
        std::this_thread::sleep_for(150ms);
        (void)cancel.cancel();
        bool cancelled = false;
        try {
            (void)pending.wait();
        } catch (const IoOperationCancelled &) {
            cancelled = true;
        }
        check(cancelled, "cancelling an h3 request resets just that stream");
        auto after = client.get(base + "/hello").wait();
        check(after && via(after).find("HTTP/3") != std::string::npos, "...and the connection keeps serving others");

        HttpClientOptions no_h3 = client_options;
        no_h3.http3 = false;
        HttpClient tcp_only(no_h3);
        (void)tcp_only.get(base + "/hello").wait();
        auto still_tcp = tcp_only.get(base + "/hello").wait();
        check(still_tcp && via(still_tcp).find("HTTP/2") != std::string::npos, "http3=false stays on TCP even after seeing Alt-Svc");

        HttpClientOptions eager = client_options;
        eager.http3_prior_knowledge = true;
        HttpClient prior(eager);
        auto direct = prior.get(base + "/hello").wait();
        check(direct && via(direct).find("HTTP/3") != std::string::npos, "prior knowledge goes straight to HTTP/3");
    }

    // ---- UDP blocked / no h3 at the origin: fall back to TCP once, then stop trying ----
    {
        HttpServerOptions plain_options;
        plain_options.bind_address = "127.0.0.1";
        plain_options.tls_certificate_chain_file = (tests_dir / "test-server-cert.pem").string();
        plain_options.tls_private_key_file = (tests_dir / "test-server-key.pem").string();
        HttpServer plain(plain_options, router.handler());
        check(plain.start().has_value(), "a server without HTTP/3 starts");
        const std::string plain_base = "https://127.0.0.1:" + std::to_string(plain.local_port().value());
        auto cert_pem = read_text_file((tests_dir / "test-server-cert.pem").string());
        HttpClientOptions options;
        options.tls.pinned_sha256 = {certificate_sha256(*cert_pem).value()};
        options.http3_prior_knowledge = true;
        options.http3_connect_timeout = 600ms;
        HttpClient client(options);
        const auto start = std::chrono::steady_clock::now();
        auto first = client.get(plain_base + "/hello").wait();
        const auto first_took = std::chrono::steady_clock::now() - start;
        check(first && first->body == "hello over h3", "the request still succeeds via TCP when QUIC does not answer");
        check(first_took >= 500ms && first_took < 3s, "...after waiting out the short QUIC timeout");
        const auto second_start = std::chrono::steady_clock::now();
        auto second = client.get(plain_base + "/hello").wait();
        check(second && std::chrono::steady_clock::now() - second_start < 400ms, "later requests do not try QUIC again");
        plain.stop();
    }

    server.stop();
    fs::remove_all(root);
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
