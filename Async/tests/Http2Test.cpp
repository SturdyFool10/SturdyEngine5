// HTTP/2: the engine serving real requests (checked against curl, an independent implementation) and as a client.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <chrono>
#include <cstdlib>
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
            Foundation::log_error("Http2Test FAILED: {}", what);
            ++failures;
        }
    }

    /// Runs curl and returns {exit code, stdout}.
    struct CurlResult {
        int code = -1;
        std::string out;
    };

    CurlResult curl(std::vector<std::string> args, std::vector<std::byte> stdin_data = {}) {
        ProcessOptions options;
        options.program = "curl";
        options.args = std::move(args);
        RunLimits limits;
        limits.timeout = 20s;
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

} // namespace

int main() {
    Scheduler::initialize(2);
    const fs::path tests_dir = fs::path(__FILE__).parent_path();
    const fs::path root = fs::temp_directory_path() / "sturdy_h2_test";
    fs::remove_all(root);
    std::string big(3 * 1024 * 1024, 'z');
    for (std::size_t i = 0; i < big.size(); i += 997) big[i] = static_cast<char>('a' + (i / 997) % 26);
    write_file(root / "big.bin", big);
    write_file(root / "small.txt", "0123456789");

    Router router;
    router.get("/hello", [](const RouteRequest &) { return text_response(200, "hello over h2"); });
    router.post("/echo", [](const RouteRequest &r) { return text_response(200, r.http.body); });
    router.get("/inspect", [](const RouteRequest &r) {
        return text_response(200, "host=" + std::string(r.header("host").value_or("-")) + " ua=" + std::string(r.header("user-agent").value_or("-")) +
                                      " cookie=" + std::string(r.header("cookie").value_or("-")) + " ver=" + std::to_string(r.http.version));
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
        response.headers.push_back({"Set-Cookie", "sid=h2session; Path=/"});
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
    options.http2_max_concurrent_streams = 50;
    HttpServer server(options, router.handler());
    check(server.start().has_value(), "TLS server starts");
    check(server.offers_http2(), "the server offers HTTP/2");
    const std::string base = "https://127.0.0.1:" + std::to_string(server.local_port().value());

    auto h2 = [&](std::string path, std::vector<std::string> extra = {}) {
        std::vector<std::string> args = {"-sk", "--http2", "--max-time", "15", "-w", "\n%{http_version}|%{http_code}|%{num_connects}"};
        args.insert(args.end(), extra.begin(), extra.end());
        args.push_back(base + path);
        return curl(std::move(args));
    };
    auto trailer = [](const std::string &out) { return out.substr(out.rfind('\n') + 1); };
    auto body_of = [](const std::string &out) { return out.substr(0, out.rfind('\n')); };

    if (std::system("curl --version >/dev/null 2>&1") != 0) {
        Foundation::log_info("Http2Test: curl not available; interop checks skipped");
    } else {
        // Basic request/response over negotiated h2.
        {
            auto r = h2("/hello");
            check(r.code == 0 && body_of(r.out) == "hello over h2" && trailer(r.out).rfind("2|200", 0) == 0, "GET over HTTP/2 (curl negotiates h2 by ALPN)");
        }
        // Request headers arrive as an HTTP/1 handler expects them.
        {
            auto r = h2("/inspect", {"-H", "User-Agent: probe/1", "-H", "Cookie: a=1", "-H", "Cookie: b=2"});
            const auto body = body_of(r.out);
            check(body.find("host=127.0.0.1:") == 0 && body.find("ua=probe/1") != std::string::npos && body.find("ver=20") != std::string::npos,
                  "pseudo-headers map to Host, regular headers pass through, version reports 2.0");
        }
        // Request bodies, including one over the limit.
        {
            auto r = h2("/echo", {"--data-binary", "@-"});
            check(r.code == 0 || r.code == 26, "curl runs with stdin (sanity)");
            const std::string payload(100 * 1024, 'q');
            auto echoed = curl({"-sk", "--http2", "-X", "POST", "--data-binary", "@-", "-w", "\n%{http_version}", base + "/echo"}, [&] {
                std::vector<std::byte> v(payload.size());
                std::memcpy(v.data(), payload.data(), payload.size());
                return v;
            }());
            check(body_of(echoed.out) == payload && trailer(echoed.out) == "2", "a 100 KiB request body round-trips");
            const std::string too_big(400 * 1024, 'x');
            std::vector<std::byte> v(too_big.size());
            std::memcpy(v.data(), too_big.data(), too_big.size());
            auto rejected = curl({"-sk", "--http2", "-X", "POST", "--data-binary", "@-", "-w", "\n%{http_code}", base + "/echo"}, std::move(v));
            check(trailer(rejected.out) == "413" || rejected.code != 0, "a request body over the limit is refused (413 or a stream reset)");
        }
        // Large and streamed responses.
        {
            auto r = h2("/big");
            check(r.code == 0 && body_of(r.out) == big && trailer(r.out).rfind("2|200", 0) == 0, "a 3 MiB response crosses HTTP/2 flow control intact");
            auto slow = h2("/slow");
            check(body_of(slow.out) == "chunk0;chunk1;chunk2;chunk3;", "a streamed (producer) response works over HTTP/2");
            auto file = h2("/files/big.bin");
            check(body_of(file.out) == big, "a file response is read on the IO lane and streamed in DATA frames");
            auto range = h2("/files/small.txt", {"-H", "Range: bytes=2-5"});
            check(body_of(range.out) == "2345" && trailer(range.out).find("|206|") != std::string::npos, "byte ranges work over HTTP/2");
            auto head = curl({"-skI", "--http2", "-w", "%{http_version}", base + "/files/small.txt"});
            check(head.out.find("content-length: 10") != std::string::npos && head.out.rfind("2") == head.out.size() - 1, "HEAD reports content-length without a body");
        }
        // Multiplexing: 20 requests, one connection.
        {
            std::vector<std::string> args = {"-sk", "--http2", "--parallel", "--parallel-max", "20", "-w", "%{num_connects},%{http_version}\n"};
            for (int i = 0; i < 20; ++i) {
                args.push_back("-o");
                args.push_back("/dev/null");
                args.push_back(base + "/delayed?ms=" + std::to_string(150));
            }
            const auto start = std::chrono::steady_clock::now();
            auto r = curl(args);
            const auto elapsed = std::chrono::steady_clock::now() - start;
            int new_connections = 0;
            for (std::size_t at = 0; at < r.out.size();) {
                auto end = r.out.find('\n', at);
                if (end == std::string::npos) break;
                if (r.out.compare(at, 1, "1") == 0) ++new_connections;
                at = end + 1;
            }
            check(r.code == 0 && new_connections == 1, "20 parallel requests share one connection");
            check(elapsed < 2s, "...and run concurrently (20 x 150 ms handlers finish in well under 3 s)");
        }
        // Security.
        {
            auto injected = h2("/inject");
            check(trailer(injected.out).find("|500|") != std::string::npos && body_of(injected.out).find("pwned") == std::string::npos, "a response header with CR/LF is refused on HTTP/2 too");
        }
        // Fallback: a client that only speaks HTTP/1.1 still works on the same port.
        {
            auto r = curl({"-sk", "--http1.1", "-w", "\n%{http_version}", base + "/hello"});
            check(body_of(r.out) == "hello over h2" && trailer(r.out) == "1.1", "HTTP/1.1 clients are still served on the same TLS port");
        }
    }

    // ---- the engine as a client: HttpClient negotiates h2 and multiplexes ----
    {
        auto cert_pem = read_text_file((tests_dir / "test-server-cert.pem").string());
        HttpClientOptions client_options;
        if (cert_pem) client_options.tls.pinned_sha256 = {certificate_sha256(*cert_pem).value()};
        client_options.cookies = true;
        HttpClient client(client_options);

        auto hello = client.get(base + "/hello").wait();
        check(hello.has_value() && hello->body == "hello over h2", "HttpClient speaks HTTP/2 to our server");
        check(hello.has_value() && header_value(hello->headers, "server").value_or("").find("HTTP/2") != std::string::npos, "...and the response really came over h2");

        // Many concurrent requests share one connection.
        const auto start = std::chrono::steady_clock::now();
        std::vector<TaskHandle<std::expected<HttpResponseMessage, IoError>>> inflight;
        for (int i = 0; i < 30; ++i) inflight.push_back(client.get(base + "/delayed?ms=200"));
        int ok = 0;
        for (auto &h : inflight) {
            auto r = h.wait();
            if (r && r->body == "waited 200") ++ok;
        }
        check(ok == 30, "30 concurrent HTTP/2 requests all succeed");
        check(std::chrono::steady_clock::now() - start < 3s, "...multiplexed (30 x 200 ms in well under 6 s)");
        const auto metrics_before = server.metrics();
        (void)client.get(base + "/hello").wait();
        check(server.metrics().connections_accepted == metrics_before.connections_accepted, "...with no extra connection for a later request (the h2 connection is reused)");

        auto big_response = client.get(base + "/big").wait();
        check(big_response && big_response->body == big, "a 3 MiB response through HttpClient over h2");
        auto streamed = client.get(base + "/slow").wait();
        check(streamed && streamed->body == "chunk0;chunk1;chunk2;chunk3;", "a streamed response through HttpClient over h2");

        const std::string upload(150 * 1024, 'u');
        auto echoed = client.post(base + "/echo", upload).wait();
        check(echoed && echoed->body == upload, "a request body goes up over h2");

        (void)client.get(base + "/cookie").wait();
        auto inspect = client.get(base + "/inspect").wait();
        check(inspect && inspect->body.find("cookie=sid=h2session") != std::string::npos, "cookies work over h2");

        HttpClientOptions tiny = client_options;
        tiny.max_response_body_bytes = 64 * 1024;
        HttpClient limited(tiny);
        check(!limited.get(base + "/big").wait().has_value(), "the response size limit applies to HTTP/2 bodies");

        CancellationSource cancel;
        auto pending = client.get(base + "/delayed?ms=3000", {}, cancel.token());
        std::this_thread::sleep_for(100ms);
        (void)cancel.cancel();
        bool cancelled = false;
        try {
            (void)pending.wait();
        } catch (const IoOperationCancelled &) {
            cancelled = true;
        }
        check(cancelled, "cancelling an h2 request resets just that stream");
        check(client.get(base + "/hello").wait().has_value(), "...and the connection keeps serving others");

        HttpClientOptions http1_only = client_options;
        http1_only.http2 = false;
        HttpClient plain(http1_only);
        auto h1 = plain.get(base + "/hello").wait();
        check(h1 && h1->body == "hello over h2" && header_value(h1->headers, "server").value_or("").find("HTTP/2") == std::string::npos, "http2=false keeps the client on HTTP/1.1");
    }

    server.stop();
    fs::remove_all(root);
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
