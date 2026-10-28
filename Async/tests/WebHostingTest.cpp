// Router, static files, HttpClient, async handlers, HTTPS and WebSocket upgrade on the HTTP server's port.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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
            Foundation::log_error("WebHostingTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> bytes(std::string_view text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }

    void write_file(const fs::path &path, std::string_view content) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
    }

    template <typename T>
    bool failed_with(const std::expected<T, IoError> &result, IoErrorCode code) {
        return !result && result.error().code == code;
    }

    std::string header_of(const HttpResponseMessage &response, std::string_view name) {
        return std::string(header_value(response.headers, name).value_or(""));
    }

    /// Sends bytes verbatim and returns everything until the server closes -- for requests HttpClient would refuse
    /// to build (path traversal attempts, odd encodings).
    std::string raw_exchange(u16 port, std::string_view request) {
        auto connection = TcpConnection::connect("127.0.0.1", port, 2s).wait().value();
        (void)connection.send_all(bytes(request)).wait();
        std::string response;
        for (;;) {
            auto chunk = connection.receive(64 * 1024).wait();
            if (!chunk || chunk->empty()) break;
            response.append(reinterpret_cast<const char *>(chunk->data()), chunk->size());
        }
        return response;
    }

    TaskHandle<void> websocket_echo(WebSocket socket) {
        for (;;) {
            auto message = co_await socket.receive();
            if (!message) co_return;
            if (!(co_await socket.send_text("echo:" + message->text()))) co_return;
        }
    }

    void routing_and_static_tests() {
        const fs::path root = fs::temp_directory_path() / "sturdy_web_test";
        fs::remove_all(root);
        write_file(root / "site/index.html", "<h1>home</h1>");
        write_file(root / "site/app.js", "console.log('hi');");
        // A real gzip sibling: raw clients see compressed bytes, the default (decoding) client sees the text.
        const bool have_gzip_tool = std::system(("printf \"console.log('hi');\" | gzip -9 > " + (root / "site/app.js.gz").string() + " 2>/dev/null").c_str()) == 0;
        write_file(root / "site/sub/page.txt", "0123456789");
        write_file(root / "site/.secret", "hidden");
        write_file(root / "outside.txt", "TOP-SECRET");
        std::string big(2 * 1024 * 1024, 'x');
        for (std::size_t i = 0; i < big.size(); i += 4099) big[i] = static_cast<char>('a' + (i / 4099) % 26);
        write_file(root / "site/big.bin", big);
        std::error_code link_error;
        fs::create_symlink(root / "outside.txt", root / "site/escape.txt", link_error);

        Router router;
        router.get("/", [](const RouteRequest &) { return html_response(200, "root"); });
        router.get("/users/:id", [](const RouteRequest &r) { return text_response(200, "user " + *r.param("id") + " q=" + r.query_param("q").value_or("-")); });
        router.get("/users/me", [](const RouteRequest &) { return text_response(200, "literal beats param"); });
        router.post("/echo", [](const RouteRequest &r) { return text_response(200, r.http.body); });
        router.get("/files/*rest", [](const RouteRequest &r) { return text_response(200, "rest=" + *r.param("rest")); });
        router.route_async("GET", "/slow", [](const RouteRequest &) {
            return then(sleep_for(300ms), [] { return text_response(200, "slow done"); });
        });
        router.get("/inject", [](const RouteRequest &) {
            HttpResponseMessage response = text_response(200, "x");
            response.headers.push_back({"X-Evil", "a\r\nSet-Cookie: pwned=1"});
            return response;
        });
        router.get("/redirect/:n", [](const RouteRequest &r) {
            const int n = std::stoi(*r.param("n"));
            return n <= 0 ? text_response(200, "arrived") : redirect_response("/redirect/" + std::to_string(n - 1));
        });
        router.post("/to-get", [](const RouteRequest &) { return redirect_response("/echo-method", 303); });
        router.route("GET", "/echo-method", [](const RouteRequest &r) { return text_response(200, r.http.method); });
        router.route("POST", "/echo-method", [](const RouteRequest &r) { return text_response(200, r.http.method); });
        router.get("/big", [](const RouteRequest &) { return text_response(200, std::string(100 * 1024, 'b')); });
        router.get("/secure-area", [](const RouteRequest &) { return text_response(200, "private"); });
        router.use([](RouteRequest &r) -> std::optional<HttpResponseMessage> {
            if (r.path == "/secure-area" && !r.header("x-token")) return text_response(401, "token required");
            return std::nullopt;
        });
        StaticFilesOptions statics;
        statics.root = (root / "site").string();
        router.static_files("/static", statics);

        HttpServerOptions options;
        options.bind_address = "127.0.0.1";
        options.handler_threads = 2;
        options.max_request_body_bytes = 64 * 1024;
        HttpServer server(options, router.handler());
        server.on_websocket("/ws", [](WebSocket socket, HttpRequestMessage) { (void)websocket_echo(std::move(socket)); });
        check(server.start().has_value(), "server starts");
        const u16 port = server.local_port().value();
        const std::string base = "http://127.0.0.1:" + std::to_string(port);

        HttpClient client;
        auto get = [&](const std::string &path, std::vector<HttpHeader> headers = {}) {
            return client.get(base + path, std::move(headers)).wait();
        };

        // ---- routing ----
        check(get("/").value().body == "root", "root route");
        check(get("/users/42?q=a%20b+c").value().body == "user 42 q=a b c", "route param + decoded query");
        check(get("/users/me").value().body == "literal beats param", "literal segment beats a parameter");
        check(get("/files/a/b/c.txt").value().body == "rest=a/b/c.txt", "wildcard captures the rest");
        {
            auto echo = client.post(base + "/echo", "payload", "text/plain").wait().value();
            check(echo.status == 200 && echo.body == "payload", "POST body reaches the handler");
            auto wrong = client.request("DELETE", base + "/echo").wait().value();
            check(wrong.status == 405 && header_of(wrong, "Allow").find("POST") != std::string::npos, "405 lists the allowed methods");
            check(get("/nope").value().status == 404, "unknown path is 404");
            auto head = client.request("HEAD", base + "/users/7").wait().value();
            check(head.status == 200 && head.body.empty() && header_of(head, "Content-Length") == "10", "HEAD returns GET's headers without a body");
            auto too_big_result = client.post(base + "/echo", std::string(200 * 1024, 'x')).wait();
            // The server may reset the connection while the client is still uploading, so a transport error is as good as a 413.
            check(!too_big_result || too_big_result->status == 413, "oversized request body is refused");
            check(get("/secure-area").value().status == 401, "middleware can stop a request");
            check(get("/secure-area", {{"X-Token", "t"}}).value().body == "private", "middleware lets authorised requests through");
        }
        check(raw_exchange(port, "GET /%zz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n").find(" 400 ") != std::string::npos, "bad percent-encoding is a 400");

        // ---- response header injection is blocked ----
        {
            auto injected = get("/inject");
            check(injected.has_value() && injected->status == 500 && header_of(*injected, "Set-Cookie").empty(), "CR/LF in a response header cannot split the response");
        }

        // ---- static files ----
        {
            auto index = get("/static/").value();
            check(index.status == 200 && index.body == "<h1>home</h1>" && header_of(index, "Content-Type").find("text/html") == 0, "directory serves index.html");
            auto js_result = get("/static/app.js");
            if (!js_result) Foundation::log_error("static js fetch failed: {} ({})", js_result.error().message, static_cast<int>(js_result.error().code));
            auto js = js_result.value();
            check(js.status == 200 && js.body == "console.log('hi');" && header_of(js, "Content-Type").find("javascript") != std::string::npos, "file with MIME type");
            check(!header_of(js, "ETag").empty() && !header_of(js, "Last-Modified").empty(), "validators are sent");
            check(get("/static/app.js", {{"If-None-Match", header_of(js, "ETag")}}).value().status == 304, "If-None-Match yields 304");
            check(get("/static/app.js", {{"If-Modified-Since", header_of(js, "Last-Modified")}}).value().status == 304, "If-Modified-Since yields 304");
            check(get("/static/app.js", {{"If-None-Match", "W/\"stale\""}}).value().status == 200, "non-matching validator serves the file");

            // A client that does not decode shows what is on the wire.
            HttpClientOptions no_decode;
            no_decode.decompress = false;
            HttpClient raw_client(no_decode);
            auto gz = raw_client.get(base + "/static/app.js", {{"Accept-Encoding", "gzip"}}).wait().value();
            if (have_gzip_tool) {
                check(gz.body.size() > 2 && static_cast<unsigned char>(gz.body[0]) == 0x1f && static_cast<unsigned char>(gz.body[1]) == 0x8b &&
                          header_of(gz, "Content-Encoding") == "gzip",
                      "precompressed sibling is served when accepted");
            }

            auto range = get("/static/sub/page.txt", {{"Range", "bytes=2-5"}}).value();
            check(range.status == 206 && range.body == "2345" && header_of(range, "Content-Range") == "bytes 2-5/10", "byte range");
            auto suffix = get("/static/sub/page.txt", {{"Range", "bytes=-3"}}).value();
            check(suffix.status == 206 && suffix.body == "789", "suffix range");
            auto open_ended = get("/static/sub/page.txt", {{"Range", "bytes=7-"}}).value();
            check(open_ended.status == 206 && open_ended.body == "789", "open-ended range");
            auto bad_range = get("/static/sub/page.txt", {{"Range", "bytes=50-60"}}).value();
            check(bad_range.status == 416 && header_of(bad_range, "Content-Range") == "bytes */10", "unsatisfiable range is 416");
            auto if_range = get("/static/sub/page.txt", {{"Range", "bytes=0-1"}, {"If-Range", "W/\"nope\""}}).value();
            check(if_range.status == 200 && if_range.body == "0123456789", "mismatched If-Range sends the whole file");

            auto whole = get("/static/big.bin").value();
            check(whole.status == 200 && whole.body == big, "2 MiB file streams intact");
            auto head = client.request("HEAD", base + "/static/big.bin").wait().value();
            check(head.body.empty() && header_of(head, "Content-Length") == std::to_string(big.size()), "HEAD on a file reports its size");
            auto middle = get("/static/big.bin", {{"Range", "bytes=1000000-1000099"}}).value();
            check(middle.status == 206 && middle.body == big.substr(1000000, 100), "range deep inside a large file");

            // Security: nothing outside the root is reachable.
            check(get("/static/.secret").value().status == 404, "dotfiles are hidden");
            check(get("/static/missing.txt").value().status == 404, "missing file");
            for (const char *attack : {"/static/../outside.txt", "/static/%2e%2e/outside.txt", "/static/sub/../../outside.txt",
                                       "/static/..%2foutside.txt", "/static/%2e%2e%2foutside.txt", "/static/sub%5c..%5c..%5coutside.txt"}) {
                const auto answer = raw_exchange(port, std::string("GET ") + attack + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
                check(answer.find("TOP-SECRET") == std::string::npos, attack);
            }
            if (!link_error) check(get("/static/escape.txt").value().status == 404, "a symlink pointing outside the root is not followed");
            check(get("/static/index.html", {}).value().status == 200, "plain file lookup still works");
        }

        // ---- client behaviour ----
        {
            auto followed = get("/redirect/3").value();
            check(followed.body == "arrived" && followed.final_url == base + "/redirect/0", "redirects are followed");
            HttpClientOptions no_follow;
            no_follow.max_redirects = 0;
            HttpClient strict(no_follow);
            auto first = strict.get(base + "/redirect/3").wait().value();
            check(first.status == 302 && header_of(first, "Location") == "/redirect/2", "redirects can be disabled");
            HttpClientOptions two;
            two.max_redirects = 2;
            HttpClient limited(two);
            check(limited.get(base + "/redirect/5").wait().value().status == 302, "redirect budget is enforced");

            auto converted = client.post(base + "/to-get", "ignored").wait().value();
            check(converted.body == "GET", "303 turns POST into GET");

            HttpClientOptions tiny;
            tiny.max_response_body_bytes = 10 * 1024;
            HttpClient small(tiny);
            check(failed_with(small.get(base + "/big").wait(), IoErrorCode::LimitExceeded), "response over the size limit fails with LimitExceeded");

            HttpClientOptions impatient;
            impatient.timeout = 100ms;
            HttpClient quick(impatient);
            check(failed_with(quick.get(base + "/slow").wait(), IoErrorCode::TimedOut), "request deadline");

            CancellationSource cancel;
            auto pending = client.get(base + "/slow", {}, cancel.token());
            std::this_thread::sleep_for(50ms);
            (void)cancel.cancel();
            bool cancelled = false;
            try {
                (void)pending.wait();
            } catch (const IoOperationCancelled &) {
                cancelled = true;
            }
            check(cancelled, "a request can be cancelled");

            check(failed_with(client.get(base + "/x", {{"X-A", "b\r\nX-Evil: 1"}}).wait(), IoErrorCode::InvalidArgument), "request header injection rejected");
            check(failed_with(client.get("ftp://x/").wait(), IoErrorCode::InvalidArgument), "unsupported scheme rejected");
            check(failed_with(client.get("http://user:pw@host/").wait(), IoErrorCode::InvalidArgument), "user-info in URL rejected");
            check(failed_with(client.get("http://127.0.0.1:1/").wait(), IoErrorCode::ConnectionRefused), "connection refused is reported");
        }

        // ---- async handlers do not pin the 2-thread handler pool ----
        {
            const auto start = std::chrono::steady_clock::now();
            std::vector<TaskHandle<std::expected<HttpResponseMessage, IoError>>> inflight;
            for (int i = 0; i < 40; ++i) inflight.push_back(client.get(base + "/slow"));
            int ok = 0;
            for (auto &h : inflight) {
                auto r = h.wait();
                if (r && r->body == "slow done") ++ok;
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            check(ok == 40, "40 concurrent slow requests all complete");
            check(elapsed < 2s, "40 x 300 ms handlers overlap on 2 handler threads (no thread is held while waiting)");
        }

        // ---- WebSocket upgrade on the HTTP port ----
        {
            auto socket = WebSocket::connect("ws://127.0.0.1:" + std::to_string(port) + "/ws", {}, {}).wait();
            check(socket.has_value(), "WebSocket upgrade on the HTTP server's port");
            if (socket) {
                (void)socket->send_text("hi").wait();
                auto reply = socket->receive().wait();
                check(reply.has_value() && reply->text() == "echo:hi", "echo over the upgraded socket");
                (void)socket->close().wait();
            }
            check(!WebSocket::connect("ws://127.0.0.1:" + std::to_string(port) + "/nope", {}, {}).wait().has_value(), "unregistered WebSocket path is not upgraded");
            check(get("/").value().body == "root", "normal HTTP still works beside WebSockets");
        }

        server.stop();
        fs::remove_all(root);
    }

    void https_tests() {
        const fs::path cert = fs::path(__FILE__).parent_path() / "test-server-cert.pem";
        const fs::path key = fs::path(__FILE__).parent_path() / "test-server-key.pem";
        auto cert_pem = read_text_file(cert.string());
        if (!cert_pem) {
            Foundation::log_info("WebHostingTest: test certificate not found; HTTPS cases skipped");
            return;
        }
        HttpServerOptions options;
        options.bind_address = "127.0.0.1";
        options.tls_certificate_chain_file = cert.string();
        options.tls_private_key_file = key.string();
        Router router;
        router.get("/", [](const RouteRequest &) { return text_response(200, "secure hello"); });
        HttpServer server(options, router.handler());
        check(server.start().has_value() && server.uses_tls(), "HTTPS server starts");
        const std::string url = "https://127.0.0.1:" + std::to_string(server.local_port().value()) + "/";

        HttpClientOptions pinned;
        pinned.tls.pinned_sha256 = {certificate_sha256(*cert_pem).value()};
        HttpClient trusting(pinned);
        auto ok = trusting.get(url).wait();
        check(ok.has_value() && ok->body == "secure hello", "HTTPS GET with a pinned certificate");
        auto second = trusting.get(url).wait();
        check(second.has_value() && second->body == "secure hello", "second request reuses the pooled TLS connection");

        HttpClient strict;
        check(failed_with(strict.get(url).wait(), IoErrorCode::TlsFailure), "HTTPS refuses an untrusted certificate by default");
        server.stop();
    }

    // ---- HttpClient: gzip, cookies, proxy -----------------------------------------------------
    /// A minimal forward proxy: answers CONNECT by tunnelling, and replies to absolute-form requests itself.
    struct ProxyStats {
        std::atomic<int> connects{0};
        std::atomic<int> absolute_requests{0};
        std::string last_authorization;
        Mutex<std::string> last_line;
    };

    TaskHandle<void> pump_bytes(std::shared_ptr<TcpConnection> from, std::shared_ptr<TcpConnection> to) {
        for (;;) {
            auto chunk = co_await from->receive(16 * 1024);
            if (!chunk || chunk->empty()) break;
            if (!(co_await to->send_all(std::move(*chunk)))) break;
        }
        to->close();
        from->close();
    }

    TaskHandle<void> proxy_session(std::shared_ptr<TcpConnection> client, ProxyStats *stats) {
        auto head = co_await client->receive_until("\r\n\r\n", 16 * 1024);
        if (!head) co_return;
        const std::string text(reinterpret_cast<const char *>(head->data()), head->size());
        const std::string line = text.substr(0, text.find("\r\n"));
        *stats->last_line.lock() = line;
        if (const auto at = text.find("Proxy-Authorization: "); at != std::string::npos) {
            stats->last_authorization = text.substr(at + 21, text.find("\r\n", at) - at - 21);
        }
        if (line.rfind("CONNECT ", 0) == 0) {
            ++stats->connects;
            const std::string authority = line.substr(8, line.find(' ', 8) - 8);
            const auto colon = authority.rfind(':');
            auto upstream = co_await TcpConnection::connect(authority.substr(0, colon), static_cast<u16>(std::stoi(authority.substr(colon + 1))), 2s);
            if (!upstream) {
                (void)co_await client->send_all(bytes("HTTP/1.1 502 Bad Gateway\r\n\r\n"));
                co_return;
            }
            auto target = std::make_shared<TcpConnection>(std::move(*upstream));
            (void)co_await client->send_all(bytes("HTTP/1.1 200 Connection established\r\n\r\n"));
            auto up = pump_bytes(client, target);
            auto down = pump_bytes(target, client);
            co_await up;
            co_await down;
        } else {
            ++stats->absolute_requests;
            const std::string body = "via proxy";
            (void)co_await client->send_all(bytes("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body));
            client->close();
        }
    }

    TaskHandle<void> proxy_accept_loop(TcpListener &listener, ProxyStats *stats) {
        for (;;) {
            auto accepted = co_await listener.accept();
            if (!accepted) co_return;
            (void)proxy_session(std::make_shared<TcpConnection>(std::move(*accepted)), stats);
        }
    }

    void client_feature_tests() {
        const fs::path root = fs::temp_directory_path() / "sturdy_client_features";
        fs::remove_all(root);
        fs::create_directories(root);

        // Pre-compressed fixtures: a small gzip body, and a ~20 MB-of-zeros bomb that deflates to a few KB.
        const char gzip_hello[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff\xcb\x48\xcd\xc9\xc9\x57\xc8\x4c\x4c\x2f\xca\x2f\xca\x49\x51\x28\xc0\x24\x00\x00\x00\xff\xff\x03\x00\x8b\xf6\x04\x8f\x1e\x00\x00\x00";
        (void)gzip_hello;
        const std::string bomb = (root / "bomb.gz").string();
        const bool have_bomb = std::system(("head -c 20000000 /dev/zero | gzip -9 > " + bomb + " 2>/dev/null").c_str()) == 0 && fs::exists(bomb);
        const std::string small_gz = (root / "small.gz").string();
        const bool have_small = std::system(("printf 'hello gzip world, hello gzip world' | gzip -9 > " + small_gz + " 2>/dev/null").c_str()) == 0 && fs::exists(small_gz);

        Router router;
        router.get("/small", [&](const RouteRequest &) {
            HttpResponseMessage response;
            response.headers.push_back({"Content-Encoding", "gzip"});
            response.headers.push_back({"Content-Type", "text/plain"});
            response.body_file = small_gz;
            return response;
        });
        router.get("/bomb", [&](const RouteRequest &) {
            HttpResponseMessage response;
            response.headers.push_back({"Content-Encoding", "gzip"});
            response.body_file = bomb;
            return response;
        });
        router.get("/plain", [](const RouteRequest &r) { return text_response(200, std::string(r.header("accept-encoding").value_or("-"))); });
        router.get("/set", [](const RouteRequest &) {
            HttpResponseMessage response = text_response(200, "set");
            response.headers.push_back({"Set-Cookie", "sid=abc123; Path=/; HttpOnly"});
            response.headers.push_back({"Set-Cookie", "pref=dark; Path=/app"});
            response.headers.push_back({"Set-Cookie", "tracker=1; Domain=127.0.0.1"});       // IP hosts cannot take Domain cookies
            response.headers.push_back({"Set-Cookie", "secure_only=1; Secure"});
            response.headers.push_back({"Set-Cookie", "short=1; Max-Age=1"});
            response.headers.push_back({"Set-Cookie", "gone=1; Expires=Thu, 01 Jan 1970 00:00:01 GMT"});
            return response;
        });
        router.get("/app/show", [](const RouteRequest &r) { return text_response(200, std::string(r.header("cookie").value_or("(none)"))); });
        router.get("/show", [](const RouteRequest &r) { return text_response(200, std::string(r.header("cookie").value_or("(none)"))); });
        router.get("/expire", [](const RouteRequest &) {
            HttpResponseMessage response = text_response(200, "expired");
            response.headers.push_back({"Set-Cookie", "sid=; Max-Age=0; Path=/"});
            return response;
        });

        HttpServerOptions options;
        options.bind_address = "127.0.0.1";
        HttpServer server(options, router.handler());
        check(server.start().has_value(), "feature server starts");
        const std::string base = "http://127.0.0.1:" + std::to_string(server.local_port().value());

        // gzip
        {
            HttpClient client;
            auto plain = client.get(base + "/plain").wait();
            check(plain && plain->body.find("gzip") != std::string::npos, "the client advertises gzip/deflate");
            if (have_small) {
                auto small = client.get(base + "/small").wait();
                check(small && small->body == "hello gzip world, hello gzip world" && !header_value(small->headers, "content-encoding"),
                      "a gzip body is decoded and its Content-Encoding header removed");
            }
            if (have_bomb) {
                HttpClientOptions limited;
                limited.max_response_body_bytes = 1024 * 1024;
                HttpClient strict(limited);
                auto result = strict.get(base + "/bomb").wait();
                check(failed_with(result, IoErrorCode::LimitExceeded), "a gzip bomb is cut off by the decoded-size limit");
                HttpClientOptions generous;
                generous.max_response_body_bytes = 64 * 1024 * 1024;
                HttpClient lenient(generous);
                auto full = lenient.get(base + "/bomb").wait();
                check(full && full->body.size() == 20'000'000, "the same body decodes fully under a larger limit");
            }
            HttpClientOptions raw;
            raw.decompress = false;
            HttpClient undecoded(raw);
            check(undecoded.get(base + "/plain").wait()->body == "identity", "decompress=false asks for identity");
        }

        // cookies
        {
            HttpClientOptions with_cookies;
            with_cookies.cookies = true;
            HttpClient client(with_cookies);
            check(client.get(base + "/show").wait()->body == "(none)", "no cookie before one is set");
            (void)client.get(base + "/set").wait();
            const std::string shown = client.get(base + "/show").wait()->body;
            check(shown.find("sid=abc123") != std::string::npos, "a session cookie is sent back");
            check(shown.find("pref=") == std::string::npos, "a Path=/app cookie is not sent to /show");
            check(shown.find("tracker") == std::string::npos, "a Domain cookie for an IP host is refused");
            check(shown.find("secure_only") == std::string::npos, "a Secure cookie is not sent over http");
            check(shown.find("gone=") == std::string::npos, "a cookie that arrives already expired is dropped");
            check(client.get(base + "/app/show").wait()->body.find("pref=dark") != std::string::npos, "the path-scoped cookie is sent under /app");
            std::this_thread::sleep_for(1200ms);
            check(client.get(base + "/show").wait()->body.find("short=") == std::string::npos, "Max-Age cookies expire");
            (void)client.get(base + "/expire").wait();
            check(client.get(base + "/show").wait()->body.find("sid=") == std::string::npos, "Max-Age=0 deletes a cookie");

            HttpClient without;
            (void)without.get(base + "/set").wait();
            check(without.get(base + "/show").wait()->body == "(none)", "a client without a jar sends no cookies");
            check(client.cookie_jar() && !client.cookie_jar()->all().empty(), "the jar can be inspected");
        }

        // proxy
        {
            auto proxy_listener = TcpListener::listen("127.0.0.1", 0).wait().value();
            ProxyStats stats;
            auto proxy_loop = proxy_accept_loop(proxy_listener, &stats);
            const std::string proxy_url = "http://user:pw@127.0.0.1:" + std::to_string(proxy_listener.local_port().value());

            HttpClientOptions through;
            through.proxy = proxy_url;
            HttpClient client(through);
            auto plain = client.get("http://example.invalid/page?x=1").wait();
            check(plain && plain->body == "via proxy", "http:// requests go to the proxy in absolute form");
            check(stats.last_line.lock()->rfind("GET http://example.invalid/page?x=1 ", 0) == 0, "the request line is absolute-form");
            check(stats.last_authorization == "Basic dXNlcjpwdw==", "Proxy-Authorization carries the proxy credentials");

            // https through a CONNECT tunnel to a real TLS server, verified end to end.
            const fs::path cert = fs::path(__FILE__).parent_path() / "test-server-cert.pem";
            const fs::path key = fs::path(__FILE__).parent_path() / "test-server-key.pem";
            if (auto cert_pem = read_text_file(cert.string())) {
                HttpServerOptions tls_options;
                tls_options.bind_address = "127.0.0.1";
                tls_options.tls_certificate_chain_file = cert.string();
                tls_options.tls_private_key_file = key.string();
                Router tls_router;
                tls_router.get("/", [](const RouteRequest &) { return text_response(200, "tunnelled hello"); });
                HttpServer tls_server(tls_options, tls_router.handler());
                check(tls_server.start().has_value(), "TLS target starts");
                const std::string target = "https://127.0.0.1:" + std::to_string(tls_server.local_port().value()) + "/";

                HttpClientOptions tunnelled;
                tunnelled.proxy = proxy_url;
                tunnelled.tls.pinned_sha256 = {certificate_sha256(*cert_pem).value()};
                HttpClient tunnel_client(tunnelled);
                auto answer = tunnel_client.get(target).wait();
                check(answer && answer->body == "tunnelled hello", "https through a CONNECT tunnel works");
                check(stats.connects >= 1, "the proxy saw a CONNECT");

                HttpClientOptions wrong_pin = tunnelled;
                wrong_pin.tls.pinned_sha256 = {std::string(64, '0')};
                HttpClient strict(wrong_pin);
                check(failed_with(strict.get(target).wait(), IoErrorCode::TlsFailure), "the tunnel does not weaken certificate verification");
                tls_server.stop();
            }

            // no_proxy sends the request straight to the origin.
            HttpClientOptions bypass;
            bypass.proxy = proxy_url;
            bypass.no_proxy = {"127.0.0.1"};
            HttpClient direct(bypass);
            const int before = stats.absolute_requests;
            auto routed = direct.get(base + "/plain").wait();
            check(routed && routed->body.find("gzip") != std::string::npos && stats.absolute_requests == before, "no_proxy bypasses the proxy");
            check(failed_with(HttpClient(HttpClientOptions{.proxy = "ftp://nope"}).get(base + "/plain").wait(), IoErrorCode::InvalidArgument) || true, "bad proxy URLs do not crash");

            proxy_listener.close();
            proxy_loop.wait();
        }
        server.stop();
        fs::remove_all(root);
    }

    // ---- server: streaming, spooled uploads, per-IP caps, metrics ----------------------------
    void server_feature_tests() {
        std::atomic<int> sse_sent{0};
        Router router;
        router.get("/chunks", [](const RouteRequest &) {
            auto parts = stream_from([n = 0]() mutable -> TaskHandle<std::optional<std::string>> {
                if (n >= 5) return ready(std::optional<std::string>{});
                const std::string piece = "part" + std::to_string(n++) + ";";
                return then(sleep_for(20ms), [piece] { return std::optional<std::string>(piece); });
            });
            return stream_response(std::move(parts), "text/plain");
        });
        struct Sentinel {
            std::atomic<bool> *flag;
            ~Sentinel() { *flag = true; }
        };
        std::atomic<bool> producer_destroyed{false};
        router.get("/events", [&](const RouteRequest &) {
            HttpResponseMessage response;
            response.headers.push_back({"Content-Type", "text/event-stream"});
            auto guard = std::make_shared<Sentinel>(Sentinel{&producer_destroyed});
            // A slow event source: each wait is long, and registers for cancellation so the server can interrupt it.
            response.body_stream = [&, guard, n = std::make_shared<int>(0)](CancellationToken cancel) -> TaskHandle<std::optional<std::string>> {
                Promise<std::optional<std::string>> promise;
                auto timer = sleep_for((*n) == 0 ? 10ms : 5s, cancel);
                timer.on_done([timer, promise, n, &sse_sent]() mutable {
                    try {
                        timer.wait();
                        ++sse_sent;
                        (void)promise.set_value(std::optional<std::string>(format_sse(SseEvent{.data = "tick " + std::to_string((*n)++)})));
                    } catch (...) {
                        (void)promise.set_exception(std::current_exception());
                    }
                });
                return promise.handle();
            };
            (void)guard;
            return response;
        });
        router.get("/sse", [](const RouteRequest &) {
            return sse_response(stream_of(std::vector<SseEvent>{{.data = "one"}, {.data = "line1\nline2", .event = "multi", .id = "7", .retry_ms = 1500}}));
        });
        router.get("/short", [](const RouteRequest &) {
            HttpResponseMessage response = text_response(200, "");
            response.body.clear();
            response.body_stream = [](CancellationToken) { return ready(std::optional<std::string>{}); };
            return response;
        });
        router.post("/upload", [](const RouteRequest &r) {
            std::string content;
            if (!r.http.body_file.empty()) {
                std::ifstream in(r.http.body_file, std::ios::binary);
                content.assign(std::istreambuf_iterator<char>(in), {});
            }
            return text_response(200, std::to_string(r.http.body.size()) + ":" + std::to_string(content.size()) + ":" + r.http.body_file);
        });
        router.get("/ok", [](const RouteRequest &) { return text_response(200, "ok"); });

        HttpServerOptions options;
        options.bind_address = "127.0.0.1";
        options.spool_requests_above = 1000;
        options.max_spooled_body_bytes = 200 * 1000;
        options.max_request_body_bytes = 64 * 1024;
        options.max_connections_per_ip = 3;
        HttpServer server(options, router.handler());
        check(server.start().has_value(), "streaming server starts");
        const u16 port = server.local_port().value();
        const std::string base = "http://127.0.0.1:" + std::to_string(port);
        HttpClient client;

        // Chunked streaming from a Stream.
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto chunks = client.get(base + "/chunks").wait();
            check(chunks && chunks->body == "part0;part1;part2;part3;part4;", "a streamed body arrives complete and in order");
            check(chunks && header_value(chunks->headers, "transfer-encoding").value_or("") == "chunked", "streamed bodies use chunked transfer encoding");
            check(std::chrono::steady_clock::now() - t0 >= 90ms, "the body really was produced over time");
        }
        // An empty producer still yields a valid, terminated response.
        {
            auto empty = client.get(base + "/short").wait();
            check(empty && empty->status == 200 && empty->body.empty(), "an empty streamed body is a valid response");
            check(client.get(base + "/ok").wait()->body == "ok", "the connection stays usable after a streamed response");
        }
        // Server-Sent Events formatting.
        {
            auto sse = client.get(base + "/sse").wait();
            check(sse && sse->body == "data: one\n\nevent: multi\nid: 7\nretry: 1500\ndata: line1\ndata: line2\n\n", "SSE events are formatted per spec");
            check(sse && header_value(sse->headers, "content-type").value_or("") == "text/event-stream", "SSE content type");
            check(format_sse(SseEvent{.data = "x", .event = "a\nevent: evil"}).find("\nevent: evil") == std::string::npos, "a newline in an event name cannot inject fields");
        }
        // A client that leaves mid-stream cancels the producer.
        {
            auto raw = TcpConnection::connect("127.0.0.1", port, 2s).wait().value();
            (void)raw.send_all(bytes("GET /events HTTP/1.1\r\nHost: x\r\n\r\n")).wait();
            (void)raw.receive_until("\r\n\r\n", 4096).wait();   // headers
            auto first = raw.receive(256).wait();
            check(first.has_value() && !first->empty(), "SSE data arrives while the stream is open");
            raw.close();
            const auto left_at = std::chrono::steady_clock::now();
            const bool noticed = [&] {
                for (int i = 0; i < 100; ++i) {
                    if (producer_destroyed) return true;
                    std::this_thread::sleep_for(10ms);
                }
                return false;
            }();
            check(noticed, "a client that leaves mid-stream cancels the 5 s wait and frees the producer (no waiting for the next write)");
            check(std::chrono::steady_clock::now() - left_at < 2s, "...promptly");
        }

        // Spooled uploads.
        {
            auto small = client.post(base + "/upload", std::string(500, 'a')).wait();
            check(small && small->body.rfind("500:0:", 0) == 0, "a small body stays in memory");
            const std::string large(50 * 1000, 'b');
            auto big = client.post(base + "/upload", large).wait();
            check(big.has_value(), "large upload succeeds");
            std::string spooled_path;
            if (big) {
                check(big->body.rfind("0:50000:/", 0) == 0, "a large body is spooled to a file and not held in memory");
                spooled_path = big->body.substr(big->body.rfind(':') + 1);
                check(fs::path(spooled_path).parent_path().filename().string().rfind("sturdy-upload-", 0) == 0, "spooled bodies live in a dedicated directory");
                std::this_thread::sleep_for(100ms);
                check(!fs::exists(spooled_path), "the spool file is deleted after the response");
            }
            auto too_big = client.post(base + "/upload", std::string(300 * 1000, 'c')).wait();
            check(!too_big || too_big->status == 413, "an upload over the spool limit is refused");
            check(client.get(base + "/ok").wait()->body == "ok", "the server is fine after refused uploads");
        }

        // Per-IP connection cap and metrics.
        {
            std::vector<TcpConnection> held;
            for (int i = 0; i < 3; ++i) held.push_back(TcpConnection::connect("127.0.0.1", port, 2s).wait().value());
            for (auto &c : held) (void)c.send_all(bytes("GET /ok HTTP/1.1\r\nHost: x\r\n\r\n")).wait();
            for (auto &c : held) (void)c.receive_until("\r\n\r\n", 4096).wait();
            auto fourth = TcpConnection::connect("127.0.0.1", port, 2s).wait();
            bool refused = !fourth.has_value();
            if (fourth) {
                (void)fourth->send_all(bytes("GET /ok HTTP/1.1\r\nHost: x\r\n\r\n")).wait();
                auto reply = fourth->receive(256).wait();
                refused = !reply.has_value() || reply->empty();
            }
            check(refused, "a fourth connection from one address is dropped");
            const auto metrics = server.metrics();
            check(metrics.connections_rejected_per_ip >= 1, "metrics count per-IP rejections");
            check(metrics.requests >= 8 && metrics.responses_2xx >= 8, "metrics count requests and 2xx responses");
            check(metrics.active_connections >= 3, "metrics report active connections");
            held.clear();
            std::this_thread::sleep_for(150ms);
            check(client.get(base + "/ok").wait().has_value(), "capacity returns when connections close");
        }
        server.stop();
    }

    void rate_limit_tests() {
        RateLimiter limiter(2.0, 3.0, 100);
        check(limiter.try_acquire("a") && limiter.try_acquire("a") && limiter.try_acquire("a"), "a burst of three is allowed");
        check(!limiter.try_acquire("a"), "the fourth immediate request is refused");
        check(limiter.retry_after("a") > 0ms && limiter.retry_after("a") <= 500ms, "retry_after says how long to wait");
        check(limiter.try_acquire("b"), "other keys are unaffected");
        std::this_thread::sleep_for(600ms);
        check(limiter.try_acquire("a") && !limiter.try_acquire("a"), "tokens refill at the configured rate (one in 600 ms at 2/s)");
        for (int i = 0; i < 1000; ++i) (void)limiter.try_acquire("flood-" + std::to_string(i));
        check(limiter.tracked_keys() <= 100, "a flood of distinct keys cannot grow memory past max_keys");

        auto shared = std::make_shared<RateLimiter>(1.0, 2.0);
        Router router;
        router.use(rate_limit_middleware(shared));
        router.get("/", [](const RouteRequest &) { return text_response(200, "ok"); });
        auto request_from = [&](const char *address) {
            HttpRequestMessage request;
            request.method = "GET";
            request.target = "/";
            request.remote_address = address;
            return router.dispatch(std::move(request));
        };
        check(request_from("10.0.0.1").status == 200 && request_from("10.0.0.1").status == 200, "requests within the burst pass");
        const auto limited = request_from("10.0.0.1");
        check(limited.status == 429 && !header_of(limited, "Retry-After").empty(), "over the limit: 429 with Retry-After");
        check(request_from("10.0.0.2").status == 200, "limits are per client address");
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    routing_and_static_tests();
    https_tests();
    client_feature_tests();
    server_feature_tests();
    rate_limit_tests();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
