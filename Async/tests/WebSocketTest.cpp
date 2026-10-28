// WebSocket client + server: echo, negotiation, rejection paths, limits, close handshake, TLS, liveness, scale.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("WebSocketTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> bytes(std::string_view text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }

    template <typename T>
    bool failed_with(const std::expected<T, IoError> &result, IoErrorCode code) {
        return !result && result.error().code == code;
    }

    /// Echoes every message back until the peer goes away; counts connections served.
    TaskHandle<void> echo_session(WebSocket socket, std::atomic<int> *served) {
        served->fetch_add(1);
        for (;;) {
            auto message = co_await socket.receive();
            if (!message) co_return;
            auto sent = message->is_text ? co_await socket.send_text(message->text()) : co_await socket.send_binary(std::move(message->data));
            if (!sent) co_return;
        }
    }

    /// Accepts forever, running one echo session per socket (sessions detach: the handle is dropped).
    TaskHandle<void> echo_server(WebSocketServer &server, std::atomic<int> *served) {
        for (;;) {
            auto socket = co_await server.accept();
            if (!socket) co_return;
            (void)echo_session(std::move(*socket), served);
        }
    }

    std::string ws_url(const WebSocketServer &server, const char *scheme, const char *path) {
        return std::string(scheme) + "://127.0.0.1:" + std::to_string(server.local_port().value()) + path;
    }

    void basic_tests() {
        WebSocketServerOptions options;
        options.bind_address = "127.0.0.1";
        options.subprotocols = {"chat.v2", "chat.v1"};
        options.allowed_origins = {"https://app.example"};
        options.paths = {"/chat"};
        options.socket.max_message_bytes = 1_MiB;
        auto server = WebSocketServer::listen(options).wait().value();
        std::atomic<int> served{0};
        auto accept_loop = echo_server(server, &served);

        // Echo, subprotocol negotiation, request info.
        WebSocketClientOptions client_options;
        client_options.subprotocols = {"chat.v1", "other"};
        client_options.headers = {{"Origin", "https://app.example"}, {"X-Token", "abc"}};
        auto client = WebSocket::connect(ws_url(server, "ws", "/chat?room=7"), client_options, {}).wait();
        check(client.has_value(), "client connects");
        if (client) {
            check(client->request().subprotocol == "chat.v1", "server picked the first mutually supported subprotocol");
            check(client->send_text("hello").wait().has_value(), "send_text");
            auto reply = client->receive().wait();
            check(reply.has_value() && reply->is_text && reply->text() == "hello", "text echo");
            check(client->send_binary(bytes(std::string_view("bin\0data", 8))).wait().has_value(), "send_binary");
            auto binary = client->receive().wait();
            check(binary.has_value() && !binary->is_text && binary->data.size() == 8, "binary echo keeps bytes");
            check(client->ping(bytes("hi")).wait().has_value(), "ping");

            // Ordering: many queued sends come back in order.
            std::vector<TaskHandle<std::expected<void, IoError>>> sends;
            for (int i = 0; i < 100; ++i) sends.push_back(client->send_text(std::to_string(i)));
            bool ordered = true;
            for (int i = 0; i < 100; ++i) {
                auto m = client->receive().wait();
                ordered = ordered && m && m->text() == std::to_string(i);
            }
            check(ordered, "100 queued messages echo in order");

            // A large (but allowed) message round trips intact.
            std::vector<std::byte> big(900 * 1024);
            for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>(i * 7);
            (void)client->send_binary(big).wait();
            auto big_back = client->receive().wait();
            check(big_back.has_value() && big_back->data == big, "900 KiB message round trips");

            // Graceful close: both sides learn the code.
            check(client->close(1000, "bye").wait().has_value(), "close handshake completes");
            check(!client->is_open(), "closed after handshake");
            check(client->close_info().has_value() && client->close_info()->code == 1000, "close code recorded");
            check(failed_with(client->receive().wait(), IoErrorCode::Closed), "receive after close fails with Closed");
        }

        // Origin not allowed, wrong path, and a plain HTTP request are all refused.
        {
            WebSocketClientOptions evil;
            evil.headers = {{"Origin", "https://evil.example"}};
            check(!WebSocket::connect(ws_url(server, "ws", "/chat"), evil, {}).wait().has_value(), "foreign Origin is rejected");
            check(!WebSocket::connect(ws_url(server, "ws", "/admin"), {}, {}).wait().has_value(), "unknown path is rejected");

            auto raw = TcpConnection::connect("127.0.0.1", server.local_port().value(), 2s).wait().value();
            (void)raw.send_all(bytes("GET /chat HTTP/1.1\r\nHost: x\r\n\r\n")).wait();
            auto head = raw.receive_until("\r\n", 256).wait();
            check(head.has_value() && std::string(reinterpret_cast<const char *>(head->data()), head->size()).find("426") != std::string::npos,
                  "a plain HTTP request gets 426 Upgrade Required");
        }

        // Oversized message: the receiver closes with 1009, the sender sees the connection end.
        {
            auto big_client = WebSocket::connect(ws_url(server, "ws", "/chat"), {}, {}).wait().value();
            (void)big_client.send_binary(std::vector<std::byte>(2 * 1024 * 1024)).wait();
            auto result = big_client.receive().wait();
            check(!result.has_value(), "message over the limit ends the connection");
        }

        // Bad URLs and header injection fail fast.
        check(failed_with(WebSocket::connect("http://x/", {}, {}).wait(), IoErrorCode::InvalidArgument), "non-ws scheme rejected");
        check(failed_with(WebSocket::connect("ws://", {}, {}).wait(), IoErrorCode::InvalidArgument), "empty authority rejected");
        WebSocketClientOptions injected;
        injected.headers = {{"X-A", "b\r\nX-Evil: 1"}};
        check(failed_with(WebSocket::connect(ws_url(server, "ws", "/chat"), injected, {}).wait(), IoErrorCode::InvalidArgument),
              "header injection is rejected");
        check(failed_with(WebSocket::connect("ws://127.0.0.1:1/", {}, {}).wait(), IoErrorCode::ConnectionRefused), "refused connection is reported");

        // Send queue cap: a fire-and-forget producer gets a clear error instead of unbounded memory.
        {
            WebSocketClientOptions small;
            small.socket.max_queued_send_bytes = 64 * 1024;
            auto limited = WebSocket::connect(ws_url(server, "ws", "/chat"), small, {}).wait().value();
            auto first = limited.send_binary(std::vector<std::byte>(60 * 1024));
            auto second = limited.send_binary(std::vector<std::byte>(60 * 1024));
            check(failed_with(second.wait(), IoErrorCode::LimitExceeded), "send beyond the queue cap fails with LimitExceeded");
            (void)first.wait();
            limited.abort();
        }

        std::this_thread::sleep_for(100ms);
        server.close();
        accept_loop.wait();
        check(served.load() >= 3, "server served the connections");
    }

    void tls_tests() {
        auto identity = generate_self_signed_identity("localhost").value();
        WebSocketServerOptions options;
        options.bind_address = "127.0.0.1";
        TlsServerOptions tls;
        tls.certificate_chain_pem = identity.certificate_pem;
        tls.private_key_pem = identity.private_key_pem;
        options.tls = tls;
        auto server = WebSocketServer::listen(options).wait().value();
        std::atomic<int> served{0};
        auto accept_loop = echo_server(server, &served);

        WebSocketClientOptions pinned;
        pinned.tls.pinned_sha256 = {identity.sha256};
        auto client = WebSocket::connect(ws_url(server, "wss", "/"), pinned, {}).wait();
        check(client.has_value(), "wss connects with a pinned certificate");
        if (client) {
            check(client->tls_info().has_value(), "tls_info available on wss");
            (void)client->send_text("secure").wait();
            auto reply = client->receive().wait();
            check(reply.has_value() && reply->text() == "secure", "echo over wss");
            (void)client->close().wait();
        }
        auto untrusted = WebSocket::connect(ws_url(server, "wss", "/"), {}, {}).wait();
        check(failed_with(untrusted, IoErrorCode::TlsFailure), "wss refuses an untrusted certificate by default");
        server.close();
        accept_loop.wait();
    }

    void liveness_tests() {
        // Healthy idle links survive thanks to automatic pings; a silent peer is dropped.
        WebSocketServerOptions options;
        options.bind_address = "127.0.0.1";
        options.socket.idle_timeout = 400ms;
        auto server = WebSocketServer::listen(options).wait().value();
        std::atomic<int> served{0};
        auto accept_loop = echo_server(server, &served);

        WebSocketClientOptions client_options;
        client_options.socket.idle_timeout = 400ms;
        auto client = WebSocket::connect(ws_url(server, "ws", "/"), client_options, {}).wait().value();
        std::this_thread::sleep_for(1500ms);
        check(client.is_open(), "idle connection stays open through keep-alive pings");
        (void)client.send_text("still here").wait();
        auto reply = client.receive().wait();
        check(reply.has_value() && reply->text() == "still here", "idle connection still works");

        // A raw peer that upgrades and then says nothing (and never answers pings) is cut off.
        auto raw = TcpConnection::connect("127.0.0.1", server.local_port().value(), 2s).wait().value();
        (void)raw.send_all(bytes("GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")).wait();
        (void)raw.receive_until("\r\n\r\n", 1024).wait();
        const auto before = std::chrono::steady_clock::now();
        auto ended = raw.receive(1024); // pings arrive (ignored), then the server gives up and closes
        for (;;) {
            auto got = ended.wait();
            if (!got || got->empty()) break;
            ended = raw.receive(1024);
        }
        check(std::chrono::steady_clock::now() - before < 3s, "a silent peer is dropped by the idle timeout");
        server.close();
        accept_loop.wait();
    }

    void compression_tests() {
        WebSocketServerOptions options;
        options.bind_address = "127.0.0.1";
        options.socket.compression = true;
        options.socket.max_message_bytes = 1_MiB;
        auto server = WebSocketServer::listen(options).wait().value();
        std::atomic<int> served{0};
        auto accept_loop = echo_server(server, &served);

        WebSocketClientOptions client_options;
        client_options.socket.compression = true;
        client_options.socket.max_message_bytes = 16_MiB; // the client itself is willing to receive big answers
        client_options.socket.max_queued_send_bytes = 128_MiB; // so the bomb below is actually sent
        auto client = WebSocket::connect(ws_url(server, "ws", "/"), client_options, {}).wait();
        check(client.has_value(), "compressed WebSocket connects");
        if (client) {
            std::vector<std::byte> compressible(600 * 1024);
            for (std::size_t i = 0; i < compressible.size(); ++i) compressible[i] = static_cast<std::byte>(i % 7);
            (void)client->send_binary(compressible).wait();
            auto echoed = client->receive().wait();
            check(echoed.has_value() && echoed->data == compressible, "a compressible message round-trips through permessage-deflate");
        }

        // A tiny compressed message that inflates far past the limit must be refused, not expanded into memory.
        auto bomber = WebSocket::connect(ws_url(server, "ws", "/"), client_options, {}).wait();
        check(bomber.has_value(), "second compressed client");
        if (bomber) {
            (void)bomber->send_binary(std::vector<std::byte>(64 * 1024 * 1024)).wait(); // 64 MiB of zeros deflates to ~64 KiB
            auto result = bomber->receive().wait();
            check(!result.has_value(), "a compression bomb over max_message_bytes ends the connection");
        }
        server.close();
        accept_loop.wait();
    }

    void scale_test() {
        WebSocketServerOptions options;
        options.bind_address = "127.0.0.1";
        options.max_connections = 512;
        auto server = WebSocketServer::listen(options).wait().value();
        std::atomic<int> served{0};
        auto accept_loop = echo_server(server, &served);

        constexpr int count = 200;
        std::vector<TaskHandle<std::expected<WebSocket, IoError>>> connects;
        for (int i = 0; i < count; ++i) connects.push_back(WebSocket::connect(ws_url(server, "ws", "/"), {}, {}));
        std::vector<WebSocket> clients;
        for (auto &h : connects) {
            auto c = h.wait();
            check(c.has_value(), "scale: connect");
            if (c) clients.push_back(std::move(*c));
        }
        std::vector<TaskHandle<std::expected<WebSocketMessage, IoError>>> replies;
        for (std::size_t i = 0; i < clients.size(); ++i) {
            (void)clients[i].send_text("m" + std::to_string(i));
            replies.push_back(clients[i].receive());
        }
        int good = 0;
        for (std::size_t i = 0; i < replies.size(); ++i) {
            auto r = replies[i].wait();
            if (r && r->text() == "m" + std::to_string(i)) ++good;
        }
        check(good == count, "scale: every socket got its own echo");
        check(IoHandler::pending_count() == 0, "scale: no IO-pool workers involved");
        clients.clear();
        server.close();
        accept_loop.wait();
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    basic_tests();
    tls_tests();
    liveness_tests();
    compression_tests();
    scale_test();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
