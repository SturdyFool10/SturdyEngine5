// Reactor TCP/UDP behaviour and TLS: verification, pinning, mutual TLS, framing helpers, cancellation, scale.
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
            Foundation::log_error("NetTlsTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> bytes(std::string_view text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }
    std::string text(const std::vector<std::byte> &data) { return std::string(reinterpret_cast<const char *>(data.data()), data.size()); }

    template <typename T>
    bool failed_with(const std::expected<T, IoError> &result, IoErrorCode code) {
        return !result && result.error().code == code;
    }

    template <typename F>
    bool throws(F &&fn) {
        try {
            fn();
        } catch (...) {
            return true;
        }
        return false;
    }

    // Echo server that upper-cases one line, for TLS round trips.
    TaskHandle<void> serve_one_line(TcpListener &listener) {
        auto accepted = co_await listener.accept();
        if (!accepted) co_return;
        TcpConnection connection = std::move(*accepted);
        auto line = co_await connection.receive_until("\n", 1024);
        if (!line) co_return;
        std::string reply = text(*line);
        for (char &c : reply) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        (void)co_await connection.send_all(bytes(reply));
        (void)co_await connection.shutdown_write();
        (void)co_await connection.receive(1); // wait for the peer to finish
    }

    void framing_tests() {
        auto listener = TcpListener::listen("127.0.0.1", 0).wait().value();
        const u16 port = listener.local_port().value();
        auto accepted = listener.accept();
        auto client = TcpConnection::connect("127.0.0.1", port, 2s).wait().value();
        auto server = accepted.wait().value();

        // receive_until across separate segments.
        auto line = server.receive_until("\r\n", 64);
        (void)client.send_all(bytes("GET / HT")).wait();
        std::this_thread::sleep_for(20ms);
        check(!line.is_done(), "receive_until waits for the delimiter");
        (void)client.send_all(bytes("TP/1.1\r\nrest")).wait();
        check(text(line.wait().value()) == "GET / HTTP/1.1\r\n", "receive_until stops after the delimiter");
        check(text(server.receive_exact(4).wait().value()) == "rest", "bytes after the delimiter are kept");

        // Unbounded input cannot grow memory: the limit trips.
        (void)client.send_all(std::vector<std::byte>(500, std::byte{'a'})).wait();
        check(failed_with(server.receive_until("\n", 100).wait(), IoErrorCode::LimitExceeded), "receive_until enforces max_bytes");
        check(server.receive_exact(400).wait().value().size() == 400, "bytes beyond the failed read stay buffered");

        // A cancelled receive never loses data that arrives meanwhile.
        CancellationSource source;
        auto doomed = client.receive(16, source.token());
        std::this_thread::sleep_for(20ms);
        (void)source.cancel();
        check(throws([&] { (void)doomed.wait(); }), "cancelled receive throws");
        (void)server.send_all(bytes("kept")).wait();
        std::this_thread::sleep_for(30ms);
        check(text(client.receive(16).wait().value()) == "kept", "data arriving after a cancel is delivered to the next receive");

        // Ordering: queued sends keep their order, receives are served FIFO.
        auto first = server.receive_exact(3);
        auto second = server.receive_exact(3);
        (void)client.send(bytes("abc"));
        (void)client.send(bytes("def"));
        check(text(first.wait().value()) == "abc" && text(second.wait().value()) == "def", "FIFO sends and receives");

        // Closing completes pending work with Closed.
        auto pending = server.receive(8);
        server.close();
        check(failed_with(pending.wait(), IoErrorCode::Closed), "close fails pending receives with Closed");
        check(failed_with(server.send_all(bytes("x")).wait(), IoErrorCode::Closed), "send on a closed connection fails with Closed");

        // Cancelling a queued (not yet started) send, on a fresh pair whose peer never reads.
        auto stuck_listener = TcpListener::listen("127.0.0.1", 0).wait().value();
        auto stuck_accept = stuck_listener.accept();
        auto stuck_client = TcpConnection::connect("127.0.0.1", stuck_listener.local_port().value(), 2s).wait().value();
        auto stuck_server = stuck_accept.wait().value();
        auto blocked_sender = stuck_client.send(std::vector<std::byte>(128 * 1024 * 1024)); // exceeds socket buffers
        CancellationSource send_cancel;
        auto queued = stuck_client.send(bytes("late"), send_cancel.token());
        std::this_thread::sleep_for(100ms);
        check(!blocked_sender.is_done(), "a large send stays pending while the peer is not reading");
        (void)send_cancel.cancel();
        check(throws([&] { (void)queued.wait(); }), "a queued send can be cancelled");
        stuck_client.close();
        check(failed_with(blocked_sender.wait(), IoErrorCode::Closed), "closing fails the in-flight send");
    }

    void tls_tests() {
        auto identity = generate_self_signed_identity("localhost").value();
        check(identity.sha256.size() == 64, "identity fingerprint is hex SHA-256");
        check(certificate_sha256(identity.certificate_pem).value() == identity.sha256, "certificate_sha256 agrees");

        TlsServerOptions server_options;
        server_options.certificate_chain_pem = identity.certificate_pem;
        server_options.private_key_pem = identity.private_key_pem;
        server_options.alpn = {"sturdy/1"};
        auto listener = TcpListener::listen_tls("127.0.0.1", 0, server_options).wait().value();
        u16 port = listener.local_port().value();
        // Failed handshakes can leave an already-dead connection in the ready queue, so each case gets a fresh listener.
        auto renew = [&] {
            listener.close();
            listener = TcpListener::listen_tls("127.0.0.1", 0, server_options).wait().value();
            port = listener.local_port().value();
        };

        renew(); // Pinned self-signed certificate.
        {
            auto server = serve_one_line(listener);
            TlsClientOptions options;
            options.pinned_sha256 = {identity.sha256};
            options.alpn = {"sturdy/1"};
            auto client = TcpConnection::connect_tls("127.0.0.1", port, options, 5s).wait();
            check(client.has_value(), "pinned connect succeeds");
            if (client) {
                check(client->is_tls(), "connection reports TLS");
                const auto info = client->tls_info().value();
                check(info.protocol == "TLSv1.3" || info.protocol == "TLSv1.2", "modern TLS negotiated");
                check(info.alpn == "sturdy/1", "ALPN negotiated");
                check(info.peer_sha256 == identity.sha256, "peer fingerprint reported");
                (void)client->send_all(bytes("hello tls\n")).wait();
                check(text(client->receive_exact(10).wait().value()) == "HELLO TLS\n", "data round-trips through TLS");
                check(client->receive(1).wait().value().empty(), "close_notify reads as clean EOF");
                client->close();
            }
            server.wait();
        }

        renew(); // Wrong pin is refused.
        {
            TlsClientOptions options;
            options.pinned_sha256 = {std::string(64, '0')};
            auto client = TcpConnection::connect_tls("127.0.0.1", port, options, 5s).wait();
            check(failed_with(client, IoErrorCode::TlsFailure), "mismatched pin is rejected");
        }

        renew(); // Unknown CA: refused by default (no insecure switch).
        {
            auto client = TcpConnection::connect_tls("localhost", port, {}, 5s).wait();
            check(failed_with(client, IoErrorCode::TlsFailure), "untrusted self-signed certificate is rejected by default");
        }

        renew(); // Trusting the certificate as a CA, with matching host name.
        {
            auto server = serve_one_line(listener);
            TlsClientOptions options;
            options.use_system_roots = false;
            options.ca_certificates_pem = identity.certificate_pem;
            auto client = TcpConnection::connect_tls("localhost", port, options, 5s).wait();
            check(client.has_value(), "CA-trusted connect with a matching host name succeeds");
            if (client) {
                (void)client->send_all(bytes("x\n")).wait();
                (void)client->receive_exact(2).wait();
                client->close();
            }
            server.wait();
        }

        renew(); // Right CA, wrong host name: refused.
        {
            TlsClientOptions options;
            options.use_system_roots = false;
            options.ca_certificates_pem = identity.certificate_pem;
            options.server_name = "not-localhost.example";
            auto client = TcpConnection::connect_tls("127.0.0.1", port, options, 5s).wait();
            check(failed_with(client, IoErrorCode::TlsFailure), "host name mismatch is rejected");
        }

        renew(); // Plain client against a TLS server never gets a usable connection (handshake garbage is dropped).
        {
            auto plain = TcpConnection::connect("127.0.0.1", port, 2s).wait();
            check(plain.has_value(), "tcp connect to a TLS port itself works");
            if (plain) {
                (void)plain->send_all(bytes("GET / HTTP/1.1\r\n\r\n")).wait();
                auto reply = plain->receive(64).wait();
                check(!reply.has_value() || reply->size() < 64, "a plaintext client is not served");
            }
        }

        listener.close();
    }

    void mutual_tls_tests() {
        auto server_identity = generate_self_signed_identity("localhost").value();
        auto client_identity = generate_self_signed_identity("client-1").value();
        auto other_identity = generate_self_signed_identity("intruder").value();

        TlsServerOptions server_options;
        server_options.certificate_chain_pem = server_identity.certificate_pem;
        server_options.private_key_pem = server_identity.private_key_pem;
        server_options.pinned_client_sha256 = {client_identity.sha256};
        server_options.require_client_certificate = true;
        auto listener = TcpListener::listen_tls("127.0.0.1", 0, server_options).wait().value();
        const u16 port = listener.local_port().value();

        auto options_for = [&](const TlsIdentity &identity) {
            TlsClientOptions options;
            options.pinned_sha256 = {server_identity.sha256};
            options.client_certificate_chain_pem = identity.certificate_pem;
            options.client_private_key_pem = identity.private_key_pem;
            return options;
        };

        // The pinned client gets through and the server learns who it is.
        auto accepted = listener.accept();
        auto good = TcpConnection::connect_tls("127.0.0.1", port, options_for(client_identity), 5s).wait();
        check(good.has_value(), "pinned client certificate connects");
        auto server_side = accepted.wait();
        check(server_side.has_value(), "server accepts the pinned client");
        if (server_side) {
            check(server_side->tls_info().value().peer_sha256 == client_identity.sha256, "server sees the client's fingerprint");
        }

        // An unknown client certificate never reaches accept(): TLS 1.3 lets the client finish its handshake, then it
        // sees the server's rejection on first use.
        auto next = listener.accept();
        auto intruder = TcpConnection::connect_tls("127.0.0.1", port, options_for(other_identity), 5s).wait();
        if (intruder) {
            auto reply = intruder->receive(1).wait();
            check(!reply.has_value(), "an unpinned client is cut off");
        }
        check(!next.is_done(), "the unpinned client was never accepted");

        // A client with no certificate at all is refused too.
        TlsClientOptions anonymous;
        anonymous.pinned_sha256 = {server_identity.sha256};
        auto nobody = TcpConnection::connect_tls("127.0.0.1", port, anonymous, 5s).wait();
        if (nobody) {
            check(!nobody->receive(1).wait().has_value(), "a client without a certificate is cut off");
        }
        check(!next.is_done(), "the anonymous client was never accepted");
        listener.close();
        (void)next.wait();
    }

    void udp_tests() {
        auto a = UdpSocket::bind("127.0.0.1", 0).wait().value();
        auto b = UdpSocket::bind("127.0.0.1", 0).wait().value();
        const u16 a_port = a.local_port().value();
        const u16 b_port = b.local_port().value();

        // Connected sockets: send() without an address, and only the peer is heard.
        check(a.connect("127.0.0.1", b_port).wait().has_value(), "udp connect");
        check(b.connect("127.0.0.1", a_port).wait().has_value(), "udp connect (other side)");
        auto incoming = b.receive_from(64);
        check(a.send(bytes("connected")).wait().value() == 9, "connected send");
        const auto datagram = incoming.wait().value();
        check(text(datagram.data) == "connected" && datagram.sender_port == a_port, "connected receive reports the peer");

        // Truncation to max_bytes, and a cancelled receive does not swallow a datagram.
        (void)a.send(bytes("0123456789")).wait();
        check(b.receive_from(4).wait().value().data.size() == 4, "datagrams are truncated to max_bytes");
        CancellationSource source;
        auto doomed = b.receive_from(64, source.token());
        std::this_thread::sleep_for(20ms);
        (void)source.cancel();
        check(throws([&] { (void)doomed.wait(); }), "cancelled udp receive throws");
        (void)a.send(bytes("after")).wait();
        std::this_thread::sleep_for(30ms);
        check(text(b.receive_from(64).wait().value().data) == "after", "datagram after a cancel is still delivered");

        // Name-based destination (resolved and cached).
        auto c = UdpSocket::bind("127.0.0.1", 0).wait().value();
        auto c_in = c.receive_from(64);
        auto d = UdpSocket::bind("127.0.0.1", 0).wait().value();
        check(d.send_to("localhost", c.local_port().value(), bytes("named")).wait().has_value() ||
                  d.send_to("127.0.0.1", c.local_port().value(), bytes("named")).wait().has_value(),
              "send_to by name or literal");
        check(text(c_in.wait().value().data) == "named", "datagram to a resolved name arrives");

        // Broadcast option and multicast group membership (skipped quietly where the sandbox has no multicast route).
        check(a.set_broadcast(true).wait().has_value(), "set_broadcast");
        UdpOptions shared;
        shared.reuse_address = true;
        auto m1 = UdpSocket::bind("", 0, shared).wait();
        check(m1.has_value(), "dual-stack wildcard bind");
        check(failed_with(a.join_multicast("10.1.2.3").wait(), IoErrorCode::InvalidArgument), "non-multicast group is rejected");
        auto listener_socket = UdpSocket::bind("0.0.0.0", 0, shared).wait().value();
        if (listener_socket.join_multicast("239.255.42.99", "127.0.0.1").wait().has_value()) {
            auto sender = UdpSocket::bind("127.0.0.1", 0).wait().value();
            (void)sender.set_multicast_loopback(true).wait();
            (void)sender.set_multicast_ttl(1).wait();
            auto heard = listener_socket.receive_from(64);
            (void)sender.send_to("239.255.42.99", listener_socket.local_port().value(), bytes("mcast")).wait();
            auto timer = timeout(std::move(heard), 500ms);
            try {
                check(text(timer.wait().value().data) == "mcast", "multicast datagram arrives");
            } catch (const TimeoutError &) {
                Foundation::log_info("NetTlsTest: multicast loopback not delivered here; skipped");
            }
        } else {
            Foundation::log_info("NetTlsTest: no multicast route; membership test skipped");
        }

        auto closing = a.receive_from(8);
        a.close();
        check(failed_with(closing.wait(), IoErrorCode::Closed), "close fails a pending udp receive with Closed");
    }

    void scale_test() {
        // Many idle connections cost no worker threads: the IO pool stays untouched and the scheduler stays free.
        auto listener = TcpListener::listen("127.0.0.1", 0, 1024).wait().value();
        const u16 port = listener.local_port().value();
        constexpr int count = 300;
        std::vector<TaskHandle<std::expected<TcpConnection, IoError>>> accepts;
        std::vector<TaskHandle<std::expected<TcpConnection, IoError>>> connects;
        for (int i = 0; i < count; ++i) accepts.push_back(listener.accept());
        for (int i = 0; i < count; ++i) connects.push_back(TcpConnection::connect("127.0.0.1", port, 5s));
        std::vector<TcpConnection> clients, servers;
        for (auto &h : connects) {
            auto c = h.wait();
            check(c.has_value(), "scale: connect");
            if (c) clients.push_back(std::move(*c));
        }
        for (auto &h : accepts) {
            auto s = h.wait();
            check(s.has_value(), "scale: accept");
            if (s) servers.push_back(std::move(*s));
        }
        // Every server side waits for data (300 pending reads), then one message each is echoed.
        std::vector<TaskHandle<std::expected<std::vector<std::byte>, IoError>>> reads;
        for (auto &s : servers) reads.push_back(s.receive_exact(4));
        check(IoHandler::pending_count() == 0, "scale: pending reads use no IO-pool slots");
        auto probe = Scheduler::spawn([] { return 7; });
        check(probe.wait() == 7, "scale: scheduler unaffected by 300 pending reads");
        for (auto &c : clients) (void)c.send_all(bytes("ping"));
        int good = 0;
        for (auto &r : reads) {
            auto v = r.wait();
            if (v && text(*v) == "ping") ++good;
        }
        check(good == count, "scale: every connection delivered its message");
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    framing_tests();
    tls_tests();
    mutual_tls_tests();
    udp_tests();
    scale_test();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
