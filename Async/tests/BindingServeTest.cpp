// The facade's HTTP serving surface: router, static files, streamed / SSE / file bodies, WebSocket upgrades on the HTTP server,
// spooled uploads, the rate limiter and the cookie jar -- all driven through the flat API a foreign language uses.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace SFT;
using namespace SFT::Async;
namespace B = SFT::Async::Binding;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("BindingServeTest FAILED: {}", what);
            ++failures;
        }
    }

    B::Bytes bytes(std::string_view text) { return B::Bytes(text.begin(), text.end()); }
    std::string text(const B::Bytes &data) { return std::string(data.begin(), data.end()); }

    B::Value finish(const B::OpPtr &op) {
        if (!op->wait(10'000)) {
            Foundation::log_error("BindingServeTest: an operation did not finish");
            std::abort();
        }
        return op->result();
    }

    std::string header(const B::Fields &fields, std::string_view name) {
        for (const auto &[key, value] : fields) {
            if (key.size() == name.size() && std::equal(key.begin(), key.end(), name.begin(), [](char a, char b) { return std::tolower(a) == std::tolower(b); })) return value;
        }
        return {};
    }

    B::HttpResponseSpec reply(std::string_view body, std::uint32_t status = 200) {
        B::HttpResponseSpec r;
        r.status = status;
        r.body = bytes(body);
        return r;
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    const fs::path scratch = fs::temp_directory_path() / "sturdy_binding_serve_test";
    fs::remove_all(scratch);
    fs::create_directories(scratch / "site" / "docs");
    { std::ofstream(scratch / "site" / "index.html") << "<h1>home</h1>"; }
    { std::ofstream(scratch / "site" / "docs" / "a.txt") << "alpha"; }
    { std::ofstream(scratch / "site" / ".secret") << "hidden"; }
    { std::ofstream(scratch / "range.bin") << "0123456789"; }

    // ---- router: parameters, wildcard, middleware, not_found, static files ----
    auto router = B::router_new();
    B::router_use(router, [](B::HttpRequestView request, B::HttpRespond respond) {
        for (const auto &[name, value] : request.headers) {
            if (name == "X-Deny") respond(reply("denied by middleware", 403));
        }
    });
    B::router_route(router, "GET", "/users/:id", [](B::HttpRequestView request, B::HttpRespond respond) {
        std::string id;
        for (const auto &[name, value] : request.params) if (name == "id") id = value;
        respond(reply("user " + id + " path=" + request.path + " query=" + request.query));
    });
    B::router_route(router, "POST", "/files/*rest", [](B::HttpRequestView request, B::HttpRespond respond) {
        std::string rest;
        for (const auto &[name, value] : request.params) if (name == "rest") rest = value;
        respond(reply("rest=" + rest + " body=" + text(request.body), 201));
    });
    B::router_not_found(router, [](B::HttpRequestView request, B::HttpRespond respond) { respond(reply("no route for " + request.path, 404)); });
    B::StaticFilesSpec files;
    files.root = (scratch / "site").string();
    files.mime_types.emplace_back(".glb", "model/gltf-binary");
    B::router_static_files(router, "/static", files);
    {
        bool threw = false;
        try {
            B::StaticFilesSpec bad;
            bad.root = (scratch / "does-not-exist").string();
            B::router_static_files(router, "/nope", bad);
        } catch (const B::BindingError &error) {
            threw = error.code() == IoErrorCode::InvalidArgument;
        }
        check(threw, "a static root that is not a directory is an InvalidArgument error");
    }

    {
        B::HttpRequestView request;
        request.method = "GET";
        request.target = "/users/42?x=1";
        auto r = B::router_dispatch(router, request);
        check(r.status == 200 && text(r.body) == "user 42 path=/users/42 query=x=1", "router_dispatch matches :id and splits the query");
        request.target = "/users/a%20b";
        check(text(B::router_dispatch(router, request).body).starts_with("user a b "), "path parameters are percent-decoded");
        request.target = "/missing";
        r = B::router_dispatch(router, request);
        check(r.status == 404 && text(r.body) == "no route for /missing", "unmatched paths reach the custom not_found handler");
        request.target = "/users/1";
        request.method = "DELETE";
        check(B::router_dispatch(router, request).status == 405, "a known path with another method is 405");
    }

    B::HttpServerSpec spec;
    spec.bind_address = "127.0.0.1";
    auto server = B::http_server_start_router(spec, router);
    const std::string base = "http://127.0.0.1:" + std::to_string(B::http_server_port(server));
    auto client = B::http_client_new({});
    auto get = [&](const std::string &path, B::Fields headers = {}) { return finish(B::http_request(client, "GET", base + path, headers, {})); };

    {
        auto r = get("/users/7?q=a+b");
        check(r.number == 200 && text(r.bytes) == "user 7 path=/users/7 query=q=a+b", "a routed request over the wire");
        r = finish(B::http_request(client, "POST", base + "/files/a/b/c.txt", {}, bytes("data")));
        check(r.number == 201 && text(r.bytes) == "rest=a/b/c.txt body=data", "the wildcard captures the rest of the path");
        r = get("/users/9", {{"X-Deny", "1"}});
        check(r.number == 403 && text(r.bytes) == "denied by middleware", "middleware answering before it returns stops the request");
        check(get("/users/9").number == 200, "middleware that does not answer lets the request through");
        check(get("/elsewhere").number == 404, "unmatched path over the wire");
        r = get("/static/index.html");
        check(r.number == 200 && text(r.bytes) == "<h1>home</h1>" && header(r.fields, "Content-Type").find("text/html") == 0, "static file with its MIME type");
        check(text(get("/static/").bytes) == "<h1>home</h1>", "a directory serves its index file");
        check(text(get("/static/docs/a.txt").bytes) == "alpha", "a nested static file");
        check(get("/static/.secret").number == 404, "dotfiles are hidden");
        check(get("/static/%2e%2e/range.bin").number != 200, "path traversal is refused");
        auto head = finish(B::http_request(client, "HEAD", base + "/static/docs/a.txt", {}, {}));
        check(head.number == 200 && head.bytes.empty(), "HEAD sends headers only");
    }

    // ---- streamed, SSE and file bodies ----
    {
        std::atomic<int> closed_seen{0};
        B::HttpServerSpec s2;
        s2.bind_address = "127.0.0.1";
        auto srv = B::http_server_start(s2, [&](B::HttpRequestView request, B::HttpRespond respond) {
            if (request.target == "/stream") {
                B::HttpResponseSpec r;
                r.stream = true;
                r.headers.emplace_back("Content-Type", "text/plain");
                auto body = respond(std::move(r));
                for (const char *piece : {"one,", "two,", "three"}) finish(B::http_body_write(body, bytes(piece)));
                B::http_body_finish(body);
            } else if (request.target == "/sse") {
                B::HttpResponseSpec r;
                r.sse = true;
                auto body = respond(std::move(r));
                B::SseEventSpec e;
                e.event = "tick";
                e.id = "7";
                e.data = "line1\nline2";
                e.retry_ms = 1500;
                finish(B::http_body_write_sse(body, e));
                e.event.clear();
                e.id.clear();
                e.retry_ms = -1;
                e.data = "second";
                finish(B::http_body_write_sse(body, e));
                B::http_body_finish(body);
            } else if (request.target == "/range") {
                B::HttpResponseSpec r;
                r.body_file = (scratch / "range.bin").string();
                r.body_offset = 2;
                r.body_length = 5;
                respond(std::move(r));
            } else if (request.target == "/both") {
                B::HttpResponseSpec r = reply("x");
                r.stream = true;
                bool refused = false;
                try {
                    respond(std::move(r));
                } catch (const B::BindingError &) {
                    refused = true;
                }
                // The failed attempt did not spend the one-shot: a valid answer still works.
                respond(reply(refused ? "refused-then-ok" : "not-refused"));
            } else if (request.target == "/hang") {
                B::HttpResponseSpec r;
                r.stream = true;
                r.stream_queue = 1;
                auto body = respond(std::move(r));
                std::thread([body, &closed_seen] {
                    auto closed = B::http_body_closed(body);
                    // Produce until the client is gone: a write must start failing, and `closed` must finish.
                    for (int i = 0; i < 400 && !closed->done(); ++i) {
                        auto w = B::http_body_write(body, bytes(std::string(1024, 'x')));
                        w->wait(2000);
                        if (w->result().status != B::OpStatus::Ok) break;
                        std::this_thread::sleep_for(10ms);
                    }
                    if (closed->wait(5000)) ++closed_seen;
                }).detach();
            }
        });
        const std::string b2 = "http://127.0.0.1:" + std::to_string(B::http_server_port(srv));
        auto r = finish(B::http_request(client, "GET", b2 + "/stream", {}, {}));
        check(r.number == 200 && text(r.bytes) == "one,two,three", "a streamed body arrives whole");
        r = finish(B::http_request(client, "GET", b2 + "/sse", {}, {}));
        check(header(r.fields, "Content-Type") == "text/event-stream" && header(r.fields, "Cache-Control") == "no-cache", "SSE sets its headers");
        check(text(r.bytes) == "event: tick\nid: 7\nretry: 1500\ndata: line1\ndata: line2\n\ndata: second\n\n", "SSE events are formatted per spec");
        r = finish(B::http_request(client, "GET", b2 + "/range", {}, {}));
        check(r.number == 200 && text(r.bytes) == "23456", "a file body honours offset and length");
        r = finish(B::http_request(client, "GET", b2 + "/both", {}, {}));
        check(text(r.bytes) == "refused-then-ok", "two body sources are refused without spending the response");

        // A client that disconnects mid-stream: the writer learns of it.
        auto raw = finish(B::tcp_connect("127.0.0.1", B::http_server_port(srv), nullptr, 3000)).object;
        check(finish(B::tcp_send(raw, bytes("GET /hang HTTP/1.1\r\nHost: x\r\n\r\n"))).status == B::OpStatus::Ok, "raw request sent");
        (void)finish(B::tcp_receive_until(raw, "\r\n\r\n", 4096));
        B::tcp_close(raw);
        for (int i = 0; i < 100 && closed_seen == 0; ++i) std::this_thread::sleep_for(50ms);
        check(closed_seen == 1, "a streamed body learns that the client went away");
        B::http_server_stop(srv);
    }

    // ---- spooled uploads ----
    {
        B::HttpServerSpec s3;
        s3.bind_address = "127.0.0.1";
        s3.spool_requests_above = 16;
        auto srv = B::http_server_start(s3, [&](B::HttpRequestView request, B::HttpRespond respond) {
            std::string content;
            if (!request.body_file.empty()) {
                std::ifstream in(request.body_file, std::ios::binary);
                content.assign(std::istreambuf_iterator<char>(in), {});
            }
            respond(reply("memory=" + std::to_string(request.body.size()) + " file=" + std::to_string(content.size())));
        });
        const std::string b3 = "http://127.0.0.1:" + std::to_string(B::http_server_port(srv));
        check(text(finish(B::http_request(client, "POST", b3 + "/", {}, bytes("tiny"))).bytes) == "memory=4 file=0", "small bodies stay in memory");
        check(text(finish(B::http_request(client, "POST", b3 + "/", {}, B::Bytes(1000, 'z'))).bytes) == "memory=0 file=1000", "large bodies are spooled to a file");
        B::http_server_stop(srv);
    }

    // ---- WebSocket upgrades on the HTTP server ----
    {
        B::HttpServerSpec s4;
        s4.bind_address = "127.0.0.1";
        s4.websocket_subprotocols = {"chat"};
        auto srv = B::http_server_create(s4, [](B::HttpRequestView, B::HttpRespond respond) { respond(reply("plain http")); });
        std::atomic<bool> saw_target{false};
        B::http_server_on_websocket(srv, "/ws", [&](B::ObjectPtr socket, B::HttpRequestView request) {
            saw_target = request.target == "/ws?room=1";
            auto m = finish(B::websocket_receive(socket));
            (void)finish(B::websocket_send_text(socket, "echo:" + text(m.bytes)));
            (void)finish(B::websocket_close(socket, 1000, "done"));
        });
        B::http_server_begin(srv);
        const int port = B::http_server_port(srv);
        B::WebSocketClientSpec client_spec;
        client_spec.subprotocols = {"chat"};
        auto ws = finish(B::websocket_connect("ws://127.0.0.1:" + std::to_string(port) + "/ws?room=1", client_spec)).object;
        check(ws != nullptr, "WebSocket upgrade on the HTTP server's port");
        check(finish(B::websocket_send_text(ws, "hello")).status == B::OpStatus::Ok, "send");
        check(text(finish(B::websocket_receive(ws)).bytes) == "echo:hello", "the handler talks to the upgraded socket");
        check(saw_target, "the handler sees the upgrade request");
        // The handler closes next; the client must read to take part in the closing handshake.
        const auto closing = finish(B::websocket_receive(ws));
        check(closing.status != B::OpStatus::Ok, "the close from the handler ends the stream");
        check(text(finish(B::http_request(client, "GET", "http://127.0.0.1:" + std::to_string(port) + "/", {}, {})).bytes) == "plain http", "ordinary requests still reach the HTTP handler");
        bool metric = false;
        for (const auto &[k, v] : B::http_server_metrics(srv)) if (k == "websocket_upgrades" && v == "1") metric = true;
        check(metric, "the upgrade is counted");
        bool info_ok = false;
        for (const auto &[k, v] : B::http_server_info(srv)) if (k == "running" && v == "1") info_ok = true;
        check(info_ok, "http_server_info reports the server running");
        B::http_server_stop(srv);
    }

    // ---- rate limiter, cookie jar, helpers ----
    {
        auto limiter = B::rate_limiter_new(1.0, 2.0, 100);
        check(B::rate_limiter_try_acquire(limiter, "a", 1) && B::rate_limiter_try_acquire(limiter, "a", 1), "the burst is allowed");
        check(!B::rate_limiter_try_acquire(limiter, "a", 1), "past the burst is refused");
        check(B::rate_limiter_retry_after_ms(limiter, "a", 1) > 0, "the refusal says how long to wait");
        check(B::rate_limiter_try_acquire(limiter, "b", 1) && B::rate_limiter_tracked_keys(limiter) == 2, "keys are independent and counted");

        auto limited = B::router_new();
        B::router_rate_limit(limited, 0.001, 1.0, 100);
        B::router_route(limited, "GET", "/", [](B::HttpRequestView, B::HttpRespond respond) { respond(reply("ok")); });
        B::HttpRequestView request;
        request.method = "GET";
        request.target = "/";
        request.remote_address = "10.0.0.9";
        const auto first = B::router_dispatch(limited, request).status;
        const auto second = B::router_dispatch(limited, request);
        check(first == 200 && second.status == 429 && !header(second.headers, "Retry-After").empty(), "router_rate_limit answers 429 with Retry-After");

        auto jar = B::cookie_jar_new();
        B::CookieSpec cookie;
        cookie.name = "sid";
        cookie.value = "abc";
        cookie.domain = "example.com";
        cookie.host_only = true;
        B::cookie_jar_add(jar, cookie);
        check(B::cookie_jar_header_for(jar, "example.com", "/", false) == "sid=abc", "the jar sends a matching cookie");
        check(B::cookie_jar_header_for(jar, "other.com", "/", false).empty(), "and only to its host");
        check(B::cookie_jar_all(jar).size() == 1, "cookie_jar_all lists it");
        B::cookie_jar_store_from_response(jar, "example.com", "/", false, {{"Set-Cookie", "theme=dark; Path=/"}});
        check(B::cookie_jar_all(jar).size() == 2, "Set-Cookie headers are stored");
        B::cookie_jar_clear(jar);
        check(B::cookie_jar_all(jar).empty(), "clear empties the jar");

        // A client sharing a jar sends what the server set.
        B::HttpServerSpec s5;
        s5.bind_address = "127.0.0.1";
        auto srv = B::http_server_start(s5, [](B::HttpRequestView request, B::HttpRespond respond) {
            if (request.target == "/set") {
                auto r = reply("set");
                r.headers.emplace_back("Set-Cookie", "session=xyz; Path=/");
                respond(std::move(r));
            } else {
                std::string cookie_header;
                for (const auto &[k, v] : request.headers) if (k == "Cookie") cookie_header = v;
                respond(reply("cookie=" + cookie_header));
            }
        });
        const std::string b5 = "http://127.0.0.1:" + std::to_string(B::http_server_port(srv));
        auto shared = B::cookie_jar_new();
        B::HttpClientSpec cs;
        cs.cookie_jar = shared;
        auto cookie_client = B::http_client_new(cs);
        (void)finish(B::http_request(cookie_client, "GET", b5 + "/set", {}, {}));
        check(text(finish(B::http_request(cookie_client, "GET", b5 + "/show", {}, {})).bytes) == "cookie=session=xyz", "a client with a jar returns cookies");
        check(B::cookie_jar_all(shared).size() == 1 && B::http_client_cookie_jar(cookie_client) != nullptr, "the jar the client was given is the one it fills");
        check(B::http_client_cookie_jar(client) == nullptr, "a client without cookies has no jar");
        B::http_server_stop(srv);

        check(B::percent_decode("a%20b+c", true) == "a b c" && B::percent_decode("a%20b+c", false) == "a b+c", "percent_decode");
        bool bad = false;
        try {
            (void)B::percent_decode("%zz", false);
        } catch (const B::BindingError &) {
            bad = true;
        }
        check(bad, "a malformed escape is an error");
        auto redirect = B::canned_response(B::ResponseKind::Redirect, 301, "/there");
        check(redirect.status == 301 && header(redirect.headers, "Location") == "/there", "canned redirect");
        check(header(B::canned_response(B::ResponseKind::Json, 200, "{}").headers, "Content-Type").find("application/json") == 0, "canned JSON");
        B::SseEventSpec e;
        e.data = "x\r\ninjected: 1";
        e.event = "a\nb";
        const std::string wire = B::format_sse(e);
        check(wire.find("\nb") == std::string::npos && wire.rfind("\n\n") == wire.size() - 2, "format_sse cannot be made to inject fields");
    }

    B::http_server_stop(server);
    fs::remove_all(scratch);
    if (failures == 0) Foundation::log_info("BindingServeTest passed");
    return failures == 0 ? 0 : 1;
}
