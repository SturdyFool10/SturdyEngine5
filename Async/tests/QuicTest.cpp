// QUIC: handshake with address validation, streams, flow control, datagrams, resets, limits, mutual TLS, timeouts.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("QuicTest FAILED: {}", what);
            ++failures;
        }
    }

    using Bytes = std::vector<std::byte>;

    Bytes bytes(std::string_view text) {
        Bytes out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }
    std::string text(const Bytes &data) { return std::string(reinterpret_cast<const char *>(data.data()), data.size()); }

    Bytes pattern(std::size_t n, unsigned seed) {
        Bytes out(n);
        std::uint32_t state = seed * 2654435761u + 12345;
        for (std::size_t i = 0; i < n; ++i) {
            state = state * 1664525u + 1013904223u;
            out[i] = static_cast<std::byte>(state >> 24);
        }
        return out;
    }

    template <typename T>
    std::optional<T> wait_for(TaskHandle<T> handle, std::chrono::milliseconds limit) {
        try {
            return timeout(std::move(handle), limit).wait();
        } catch (const TimeoutError &) {
            return std::nullopt;
        }
    }

    bool wait_until(const std::function<bool()> &condition, std::chrono::milliseconds limit = 5000ms) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return condition();
    }

    /// Reads a stream to its end.
    Bytes read_all(QuicStream &stream, std::chrono::milliseconds limit = 10'000ms) {
        Bytes out;
        for (;;) {
            auto chunk = wait_for(stream.receive(1 << 16), limit);
            if (!chunk || !*chunk || (*chunk)->empty()) break;
            out.insert(out.end(), (**chunk).begin(), (**chunk).end());
        }
        return out;
    }

    // ---- a small echo server -------------------------------------------------------------------------
    struct EchoServer {
        QuicListener listener;
        std::mutex lock;
        std::vector<QuicConnection> connections;
        std::atomic<bool> stopping{false};
        std::atomic<int> streams_seen{0};
    };

    TaskHandle<void> echo_stream(QuicStream stream, std::shared_ptr<EchoServer> server) {
        ++server->streams_seen;
        for (;;) {
            auto chunk = co_await stream.receive(1 << 16);
            if (!chunk) co_return;
            if (chunk->empty()) break;
            auto sent = co_await stream.send(std::move(*chunk));
            if (!sent) co_return;
        }
        if (stream.is_bidirectional()) (void)(co_await stream.finish());
    }

    TaskHandle<void> serve_connection(QuicConnection connection, std::shared_ptr<EchoServer> server) {
        {
            std::lock_guard guard(server->lock);
            server->connections.push_back(connection);
        }
        auto datagrams = [](QuicConnection c) -> TaskHandle<void> {
            for (;;) {
                auto datagram = co_await c.receive_datagram();
                if (!datagram) co_return;
                (void)c.send_datagram(std::move(*datagram));
            }
        }(connection);
        (void)datagrams;
        for (;;) {
            auto stream = co_await connection.accept_stream();
            if (!stream) co_return;
            (void)echo_stream(std::move(*stream), server);
        }
    }

    TaskHandle<void> accept_loop(std::shared_ptr<EchoServer> server) {
        for (;;) {
            auto connection = co_await server->listener.accept();
            if (!connection) co_return;
            (void)serve_connection(std::move(*connection), server);
        }
    }

    std::shared_ptr<EchoServer> start_server(const TlsIdentity &identity, std::function<void(QuicServerOptions &)> tweak = {}) {
        QuicServerOptions options;
        options.bind_address = "127.0.0.1";
        options.tls.certificate_chain_pem = identity.certificate_pem;
        options.tls.private_key_pem = identity.private_key_pem;
        options.tls.alpn = {"sturdy-test"};
        if (tweak) tweak(options);
        auto listener = QuicListener::listen(options).wait();
        if (!listener) {
            Foundation::log_error("QuicTest: cannot listen: {}", listener.error().message);
            std::abort();
        }
        auto server = std::make_shared<EchoServer>();
        server->listener = std::move(*listener);
        (void)accept_loop(server);
        return server;
    }

    QuicClientOptions client_options(const TlsIdentity &server_identity) {
        QuicClientOptions options;
        options.tls.pinned_sha256 = {server_identity.sha256};
        options.tls.alpn = {"sturdy-test"};
        return options;
    }

    std::expected<QuicConnection, IoError> dial(const EchoServer &server, QuicClientOptions options) {
        auto result = QuicConnection::connect("127.0.0.1", server.listener.local_port().value(), std::move(options)).wait();
        if (!result) Foundation::log_info("QuicTest: connect failed: {}", result.error().message);
        return result;
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    if (!quic_available()) {
        Foundation::log_info("QuicTest: QUIC not built in; skipped");
        Scheduler::shutdown();
        return 0;
    }
    auto identity = generate_self_signed_identity("localhost");
    check(identity.has_value(), "server identity");
    identity->sha256 = *certificate_sha256(identity->certificate_pem);

    // ---- handshake, address validation, basic echo ----
    {
        auto server = start_server(*identity);
        auto connection = dial(*server, client_options(*identity));
        check(connection.has_value(), "QUIC handshake succeeds");
        if (connection) {
            const auto info = connection->tls_info();
            check(info && info->protocol == "TLSv1.3" && info->alpn == "sturdy-test", "TLS 1.3 with the agreed ALPN");
            check(info && info->peer_sha256 == identity->sha256, "the peer certificate digest is the server's");
            check(server->listener.metrics().retries_sent >= 1, "the server validated the client address with a Retry");
            check(wait_until([&] { return server->listener.metrics().connections_accepted == 1; }), "server counts the connection");

            auto stream = connection->open_stream().wait();
            check(stream.has_value(), "open a stream");
            if (stream) {
                check(stream->is_bidirectional() && stream->id() == 0, "first client bidi stream is id 0");
                check(stream->send(bytes("hello quic")).wait().has_value(), "send on the stream");
                check(stream->finish().wait().has_value(), "finish the stream");
                check(text(read_all(*stream)) == "hello quic", "the echo comes back and ends cleanly");
            }

            // Datagrams.
            check(connection->max_datagram_size() > 500, "datagrams fit in a packet");
            check(!connection->send_datagram(Bytes(connection->max_datagram_size() + 100)).has_value(), "an oversized datagram is refused");
            check(connection->send_datagram(bytes("ping-datagram")).has_value(), "send a datagram");
            auto echoed = wait_for(connection->receive_datagram(), 5000ms);
            check(echoed && *echoed && text(**echoed) == "ping-datagram", "the datagram is echoed");
            int received = 0;
            for (int i = 0; i < 50; ++i) (void)connection->send_datagram(pattern(200, static_cast<unsigned>(i)));
            for (int i = 0; i < 50; ++i) {
                auto d = wait_for(connection->receive_datagram(), 1000ms);
                if (d && *d) ++received;
            }
            check(received >= 45, "nearly all of 50 datagrams make the loopback round trip");

            const auto stats = connection->stats();
            check(stats.bytes_sent > 0 && stats.smoothed_rtt.count() >= 0, "stats are reported");
            connection->close(0, "done");
        }
    }

    // ---- large transfers, flow control and many streams ----
    {
        auto server = start_server(*identity);
        auto options = client_options(*identity);
        options.stream_receive_window = 128 * 1024;
        options.connection_receive_window = 512 * 1024;
        auto connection = dial(*server, options);
        check(connection.has_value(), "handshake with small windows");
        if (connection) {
            const Bytes blob = pattern(6 * 1024 * 1024, 7);
            auto stream = connection->open_stream().wait();
            check(stream.has_value(), "open the big stream");
            auto sender = [](QuicStream s, const Bytes &data) -> TaskHandle<bool> {
                for (std::size_t offset = 0; offset < data.size(); offset += 64 * 1024) {
                    const std::size_t n = (std::min<std::size_t>)(64 * 1024, data.size() - offset);
                    auto sent = co_await s.send(Bytes(data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + n)));
                    if (!sent) co_return false;
                }
                co_return (co_await s.finish()).has_value();
            }(*stream, blob);
            const auto start = std::chrono::steady_clock::now();
            const Bytes back = read_all(*stream, 30'000ms);
            check(sender.wait(), "6 MiB sent under flow control");
            check(back == blob, "6 MiB echoed byte for byte");
            Foundation::log_info("QuicTest: 6 MiB echo took {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());

            // 40 streams at once, each with its own payload.
            std::vector<QuicStream> streams;
            std::vector<Bytes> payloads;
            for (unsigned i = 0; i < 40; ++i) {
                auto s = connection->open_stream().wait();
                if (!s) break;
                payloads.push_back(pattern(10'000 + i * 137, 100 + i));
                (void)s->send(payloads.back());
                (void)s->finish();
                streams.push_back(std::move(*s));
            }
            check(streams.size() == 40, "40 streams opened");
            bool all_good = true;
            for (std::size_t i = 0; i < streams.size(); ++i) all_good = all_good && read_all(streams[i]) == payloads[i];
            check(all_good, "40 concurrent streams each echo their own data");

            // Unidirectional: the server reads and echoes nothing back (echo_stream does not finish uni streams).
            const int before = server->streams_seen;
            auto uni = connection->open_stream(false).wait();
            check(uni && !uni->is_bidirectional(), "open a unidirectional stream");
            if (uni) {
                check(uni->send(bytes("one-way")).wait().has_value() && uni->finish().wait().has_value(), "send on it");
                check(wait_until([&] { return server->streams_seen > before; }), "the server accepted the unidirectional stream");
            }
        }
    }

    // ---- reset / stop_sending / close codes ----
    {
        auto server = start_server(*identity);
        auto connection = dial(*server, client_options(*identity));
        check(connection.has_value(), "handshake for reset tests");
        if (connection) {
            auto stream = connection->open_stream().wait();
            check(stream->send(bytes("partial")).wait().has_value(), "partial send");
            auto echoed = wait_for(stream->receive(100), 5000ms);
            check(echoed && *echoed && text(**echoed) == "partial", "partial echo");
            stream->reset(42);
            // The server echo loop sees the reset on its receive and gives up; the client's own read side is unaffected.
            auto second = connection->open_stream().wait();
            stream->stop_sending(9);
            auto stopped = wait_for(stream->receive(10), 2000ms);
            check(stopped.has_value() && !stopped->has_value(), "reading after stop_sending fails");
            check(!stream->send(bytes("late")).wait().has_value(), "sending after reset fails");
            check(second && second->send(bytes("still works")).wait().has_value(), "other streams are unaffected by a reset");

            // Close with a code: the server connection sees it.
            wait_until([&] {
                std::lock_guard guard(server->lock);
                return server->connections.size() == 1;
            });
            QuicConnection server_side;
            {
                std::lock_guard guard(server->lock);
                server_side = server->connections.front();
            }
            auto server_closed = server_side.closed();
            connection->close(7, "bye");
            auto info = wait_for(std::move(server_closed), 5000ms);
            check(info && info->from_peer && info->code == 7 && info->reason == "bye" && info->application, "the peer's close code and reason arrive");
            auto client_info = wait_for(connection->closed(), 2000ms);
            check(client_info && !client_info->from_peer && client_info->code == 7, "the closer sees its own code");
            check(!connection->is_open(), "closed connections report it");
            check(!connection->open_stream().wait().has_value(), "no streams on a closed connection");
            check(wait_for(server_side.accept_stream(), 2000ms).has_value(), "accept_stream on a closed connection completes (with an error)");
        }
    }

    // ---- stream limits: the third stream waits for the peer to grant more ----
    {
        auto server = start_server(*identity, [](QuicServerOptions &o) { o.max_bidirectional_streams = 2; });
        auto connection = dial(*server, client_options(*identity));
        check(connection.has_value(), "handshake for limit tests");
        if (connection) {
            auto a = connection->open_stream().wait();
            auto b = connection->open_stream().wait();
            check(a && b, "two streams fit the limit");
            (void)a->send(bytes("a"));
            (void)b->send(bytes("b"));
            auto third = connection->open_stream();
            check(!wait_for(third, 300ms).has_value(), "the third stream waits");
            (void)a->finish();
            (void)read_all(*a);
            auto opened = wait_for(third, 5000ms);
            check(opened && *opened && opened->has_value(), "...and opens once the peer frees a slot");
        }
    }

    // ---- TLS failures ----
    {
        auto server = start_server(*identity);
        auto wrong = client_options(*identity);
        wrong.tls.pinned_sha256 = {std::string(64, 'e')};
        auto refused = dial(*server, wrong);
        check(!refused && refused.error().code == IoErrorCode::TlsFailure, "a wrong pinned certificate fails the handshake");

        auto bad_alpn = client_options(*identity);
        bad_alpn.tls.alpn = {"something-else"};
        check(!dial(*server, bad_alpn).has_value(), "an ALPN the server does not speak fails");

        auto none = client_options(*identity);
        none.tls.alpn.clear();
        check(!dial(*server, none).has_value(), "ALPN is mandatory");

        auto unverified = client_options(*identity);
        unverified.tls.pinned_sha256.clear(); // no pin and a self-signed server: chain validation must refuse
        unverified.tls.use_system_roots = true;
        auto rejected = dial(*server, unverified);
        check(!rejected && rejected.error().code == IoErrorCode::TlsFailure, "verification is on by default (self-signed server rejected)");
        check(wait_until([&] { return server->listener.metrics().handshakes_failed >= 1; }), "the server counts failed handshakes");
    }

    // ---- mutual TLS by pinned identity ----
    {
        auto client_identity = generate_self_signed_identity("client");
        client_identity->sha256 = *certificate_sha256(client_identity->certificate_pem);
        auto server = start_server(*identity, [&](QuicServerOptions &o) {
            o.tls.require_client_certificate = true;
            o.tls.pinned_client_sha256 = {client_identity->sha256};
        });
        auto anonymous = dial(*server, client_options(*identity));
        // (A client that offered no certificate cannot know the server will reject it: TLS 1.3 lets it finish first, so the
        // refusal may arrive as an immediate close instead of a failed connect.)
        const bool refused = !anonymous.has_value() || wait_for(anonymous->closed(), 3000ms).has_value();
        check(refused, "a client without a certificate is refused when one is required");
        auto options = client_options(*identity);
        options.tls.client_certificate_chain_pem = client_identity->certificate_pem;
        options.tls.client_private_key_pem = client_identity->private_key_pem;
        auto connection = dial(*server, options);
        check(connection.has_value(), "the pinned client is accepted");
        if (connection) {
            wait_until([&] {
                std::lock_guard guard(server->lock);
                return !server->connections.empty();
            });
            std::lock_guard guard(server->lock);
            check(!server->connections.empty(), "the server holds the connection");
            if (!server->connections.empty()) {
                auto info = server->connections.front().tls_info();
                check(info && info->peer_sha256 == client_identity->sha256, "the server learns the client's certificate digest");
            }
        }
    }

    // ---- idle timeout and keep-alive ----
    {
        auto server = start_server(*identity, [](QuicServerOptions &o) { o.idle_timeout = 600ms; });
        auto idle = client_options(*identity);
        idle.idle_timeout = 600ms;
        auto quiet = dial(*server, idle);
        check(quiet.has_value(), "handshake for idle test");
        if (quiet) {
            auto info = wait_for(quiet->closed(), 5000ms);
            check(info && info->timed_out, "a silent connection times out");
        }
        auto alive = client_options(*identity);
        alive.idle_timeout = 600ms;
        alive.keep_alive = 150ms;
        auto kept = dial(*server, alive);
        check(kept.has_value(), "handshake for keep-alive test");
        if (kept) {
            std::this_thread::sleep_for(1800ms);
            check(kept->is_open(), "keep-alive holds an idle connection open past the idle timeout");
            kept->close();
        }
    }

    // ---- listener limits and shutdown ----
    {
        auto server = start_server(*identity, [](QuicServerOptions &o) { o.max_connections = 1; });
        auto first = dial(*server, client_options(*identity));
        check(first.has_value(), "first connection fits");
        auto second_options = client_options(*identity);
        second_options.handshake_timeout = 2000ms;
        auto second = dial(*server, second_options);
        check(!second.has_value(), "a connection over max_connections is refused");
        check(server->listener.metrics().rejected_limit >= 1, "...and counted");

        auto closed = first ? first->closed() : TaskHandle<QuicCloseInfo>{};
        server->listener.stop();
        if (first) {
            auto info = wait_for(std::move(closed), 5000ms);
            check(info.has_value(), "stopping the listener closes its connections");
        }
    }

    // ---- no address validation: works too ----
    {
        auto server = start_server(*identity, [](QuicServerOptions &o) { o.require_address_validation = false; });
        auto connection = dial(*server, client_options(*identity));
        check(connection.has_value() && server->listener.metrics().retries_sent == 0, "handshake without Retry");
    }

    // ---- back-pressure: an unread stream stalls the sender instead of buffering without bound ----
    {
        auto server = start_server(*identity, [](QuicServerOptions &o) {
            o.stream_receive_window = 64 * 1024;
            o.connection_receive_window = 128 * 1024;
        });
        auto options = client_options(*identity);
        options.stream_send_buffer = 128 * 1024;
        auto connection = dial(*server, options);
        check(connection.has_value(), "handshake for back-pressure");
        if (connection) {
            // A server stream that is accepted but never read.
            auto stream = connection->open_stream(false).wait();
            // (a handle's result can be consumed once, so completion is only counted, never waited on twice)
            auto completed = std::make_shared<std::atomic<int>>(0);
            for (int i = 0; i < 20; ++i) stream->send(Bytes(64 * 1024, std::byte{1})).on_done([completed] { ++*completed; });
            std::this_thread::sleep_for(300ms);
            check(*completed >= 1 && *completed < 20, "sends beyond the window and buffer stay pending");
            stream->reset(0);
            check(wait_until([&] { return *completed == 20; }), "...and fail promptly once the stream is reset");
        }
    }

    // ---- cancellation ----
    {
        auto server = start_server(*identity);
        auto connection = dial(*server, client_options(*identity));
        if (connection) {
            CancellationSource stop;
            auto pending = connection->accept_stream(stop.token());
            std::this_thread::sleep_for(100ms);
            (void)stop.cancel();
            auto result = wait_for(std::move(pending), 2000ms);
            check(result.has_value() && !result->has_value(), "cancelling accept_stream completes it");
        }
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
