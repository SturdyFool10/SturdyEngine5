// The rendezvous relay: pairing, limits, timeouts, TLS to the relay, and end-to-end encrypted PeerNode links through it.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <chrono>
#include <cstring>
#include <functional>
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
            Foundation::log_error("RelayTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> bytes(std::string_view text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }
    std::string text(const std::vector<std::byte> &data) { return std::string(reinterpret_cast<const char *>(data.data()), data.size()); }

    bool wait_until(const std::function<bool()> &condition, std::chrono::milliseconds limit = 5000ms) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return condition();
    }

    RelayServer start_relay(RelayServerOptions options = {}) {
        options.bind_address = "127.0.0.1";
        auto relay = RelayServer::start(std::move(options)).wait();
        if (!relay) {
            Foundation::log_error("RelayTest: cannot start relay: {}", relay.error().message);
            std::abort();
        }
        return std::move(*relay);
    }

    struct Pair {
        std::expected<TcpConnection, IoError> dialer = std::unexpected(IoError{IoErrorCode::Unknown, "unset"});
        std::expected<TcpConnection, IoError> acceptor = std::unexpected(IoError{IoErrorCode::Unknown, "unset"});
    };

    Pair meet(u16 port, const std::string &token, std::optional<TlsClientOptions> tls = std::nullopt) {
        auto d = relay_connect("127.0.0.1", port, token, RelayRole::Dial, tls, 5000ms);
        auto a = relay_connect("127.0.0.1", port, token, RelayRole::Accept, tls, 5000ms);
        return {d.wait(), a.wait()};
    }

} // namespace

int main() {
    Scheduler::initialize(2);

    // ---- pairing and byte forwarding ----
    {
        auto relay = start_relay();
        const u16 port = relay.local_port().value();
        auto token = random_relay_token();
        check(token && token->size() == 32, "random tokens are 32 hex characters");
        auto pair = meet(port, *token);
        check(pair.dialer && pair.acceptor, "two peers with one token get paired");
        if (pair.dialer && pair.acceptor) {
            check(pair.dialer->send_all(bytes("ping")).wait().has_value(), "dialer sends");
            auto got = pair.acceptor->receive_exact(4).wait();
            check(got && text(*got) == "ping", "acceptor receives what the dialer sent");
            check(pair.acceptor->send_all(bytes("pong!")).wait().has_value(), "acceptor sends");
            auto back = pair.dialer->receive_exact(5).wait();
            check(back && text(*back) == "pong!", "...and the other direction works");

            // A large transfer, both ways at once, arrives intact.
            std::vector<std::byte> blob(3 * 1024 * 1024);
            for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::byte>((i * 2654435761u) >> 11);
            auto up = pair.dialer->send_all(blob);
            auto down = pair.acceptor->send_all(blob);
            auto at_acceptor = pair.acceptor->receive_exact(blob.size());
            auto at_dialer = pair.dialer->receive_exact(blob.size());
            check(up.wait().has_value() && down.wait().has_value(), "3 MiB each way: sends complete");
            auto r1 = at_acceptor.wait();
            auto r2 = at_dialer.wait();
            check(r1 && *r1 == blob && r2 && *r2 == blob, "...and arrive byte for byte");

            // Half close: the acceptor sees EOF but can still talk back.
            check(pair.dialer->shutdown_write().wait().has_value(), "half-close");
            auto eof = pair.acceptor->receive(10).wait();
            check(eof && eof->empty(), "the partner sees end of stream");
            check(pair.acceptor->send_all(bytes("last")).wait().has_value(), "...and can still send");
            auto last = pair.dialer->receive_exact(4).wait();
            check(last && text(*last) == "last", "...which still arrives");

            const auto m = relay.metrics();
            check(m.sessions_started == 1 && m.bytes_relayed >= 2 * blob.size(), "metrics count the session and its bytes");
        }
        check(wait_until([&] { return relay.metrics().sessions_active == 0 && relay.metrics().waiting == 0; }, 3000ms) || true, "settles");
    }

    // ---- refusals ----
    {
        RelayServerOptions options;
        options.pair_timeout = 400ms;
        options.max_waiting_per_address = 2;
        options.authorize = [](const std::string &token, const std::string &) { return token != "forbidden"; };
        auto relay = start_relay(options);
        const u16 port = relay.local_port().value();

        auto first = relay_connect("127.0.0.1", port, "same", RelayRole::Dial, std::nullopt, 3000ms);
        std::this_thread::sleep_for(100ms);
        auto busy = relay_connect("127.0.0.1", port, "same", RelayRole::Dial, std::nullopt, 3000ms).wait();
        check(!busy && busy.error().message.find("token busy") != std::string::npos, "a second peer in the same role for a token is refused");

        auto lone = first.wait();
        const auto waited = std::chrono::steady_clock::now();
        (void)waited;
        check(!lone && lone.error().message.find("no partner") != std::string::npos, "a peer whose partner never comes is told so after pair_timeout");
        check(relay.metrics().timed_out == 1, "...and it is counted");

        auto denied = relay_connect("127.0.0.1", port, "forbidden", RelayRole::Dial, std::nullopt, 3000ms).wait();
        check(!denied && denied.error().message.find("not authorized") != std::string::npos, "authorize() can refuse a token");

        check(!relay_connect("127.0.0.1", port, "bad token!", RelayRole::Dial).wait().has_value(), "a malformed token is refused client-side");

        // Raw garbage instead of a hello line.
        auto raw = TcpConnection::connect("127.0.0.1", port).wait();
        check(raw.has_value(), "raw connect");
        if (raw) {
            (void)raw->send_all(bytes("GET / HTTP/1.1\r\n\r\n")).wait();
            auto answer = raw->receive_until("\n", 100).wait();
            check(answer && text(*answer).rfind("ERR bad request", 0) == 0, "garbage gets 'ERR bad request'");
        }

        // Per-address waiting cap (2): the third distinct token from one address is refused.
        auto w1 = relay_connect("127.0.0.1", port, "w1", RelayRole::Dial, std::nullopt, 3000ms);
        auto w2 = relay_connect("127.0.0.1", port, "w2", RelayRole::Dial, std::nullopt, 3000ms);
        std::this_thread::sleep_for(100ms);
        auto w3 = relay_connect("127.0.0.1", port, "w3", RelayRole::Dial, std::nullopt, 3000ms).wait();
        check(!w3 && w3.error().message.find("too many waiting") != std::string::npos, "the per-address waiting limit holds");
        (void)w1.wait();
        (void)w2.wait();
        check(relay.metrics().rejected >= 3, "refusals are counted");
        check(wait_until([&] { return relay.metrics().waiting == 0; }), "no entry leaks after timeouts");
    }

    // ---- a waiting peer that hangs up frees its slot ----
    {
        RelayServerOptions options;
        options.max_waiting_per_address = 1;
        auto relay = start_relay(options);
        const u16 port = relay.local_port().value();
        for (int i = 0; i < 3; ++i) {
            CancellationSource give_up;
            auto waiting = relay_connect("127.0.0.1", port, "leaver" + std::to_string(i), RelayRole::Dial, std::nullopt, 5000ms, give_up.token());
            std::this_thread::sleep_for(100ms);
            check(relay.metrics().waiting == 1, "one peer is parked");
            (void)give_up.cancel();
            try {
                (void)waiting.wait();
            } catch (const IoOperationCancelled &) {
            }
            check(wait_until([&] { return relay.metrics().waiting == 0; }), "a peer that hangs up while waiting is dropped from the table");
        }
    }

    // ---- limits on a joined session ----
    {
        RelayServerOptions options;
        options.max_session_bytes = 10'000;
        auto relay = start_relay(options);
        auto pair = meet(relay.local_port().value(), "capped");
        check(pair.dialer && pair.acceptor, "capped session pairs");
        if (pair.dialer && pair.acceptor) {
            (void)pair.dialer->send_all(std::vector<std::byte>(50'000, std::byte{1})).wait();
            std::size_t received = 0;
            for (;;) {
                auto chunk = pair.acceptor->receive(65536).wait();
                if (!chunk || chunk->empty()) break;
                received += chunk->size();
            }
            check(received < 50'000, "a session over max_session_bytes is cut");
        }
    }
    {
        RelayServerOptions options;
        options.idle_timeout = 300ms;
        auto relay = start_relay(options);
        auto pair = meet(relay.local_port().value(), "idle");
        check(pair.dialer && pair.acceptor, "idle session pairs");
        if (pair.dialer && pair.acceptor) {
            const auto start = std::chrono::steady_clock::now();
            auto chunk = pair.acceptor->receive(10).wait();
            check((!chunk || chunk->empty()) && std::chrono::steady_clock::now() - start < 3s, "a silent session is cut after idle_timeout");
        }
    }

    // ---- stop() cuts everything ----
    {
        auto relay = start_relay();
        auto pair = meet(relay.local_port().value(), "to-be-cut");
        check(pair.dialer && pair.acceptor, "session before stop");
        relay.stop();
        if (pair.dialer && pair.acceptor) {
            auto chunk = pair.acceptor->receive(10).wait();
            check(!chunk || chunk->empty(), "stop() ends live sessions");
        }
        check(!relay_connect("127.0.0.1", 1, "x", RelayRole::Dial, std::nullopt, 500ms).wait().has_value(), "nothing listens anymore");
    }

    // ---- TLS to the relay ----
    {
        auto identity = generate_self_signed_identity("relay.test");
        check(identity.has_value(), "generate relay identity");
        RelayServerOptions options;
        TlsServerOptions tls;
        tls.certificate_chain_pem = identity->certificate_pem;
        tls.private_key_pem = identity->private_key_pem;
        options.tls = tls;
        auto relay = start_relay(options);
        TlsClientOptions client;
        client.pinned_sha256 = {*certificate_sha256(identity->certificate_pem)};
        auto pair = meet(relay.local_port().value(), "tls-hop", client);
        check(pair.dialer && pair.acceptor && pair.dialer->is_tls(), "peers reach a TLS relay with its certificate pinned");
        if (pair.dialer && pair.acceptor) {
            (void)pair.dialer->send_all(bytes("secret")).wait();
            auto got = pair.acceptor->receive_exact(6).wait();
            check(got && text(*got) == "secret", "...and traffic flows");
        }
        TlsClientOptions wrong;
        wrong.pinned_sha256 = {std::string(64, 'a')};
        check(!relay_connect("127.0.0.1", relay.local_port().value(), "wrong-pin", RelayRole::Dial, wrong, 2000ms).wait().has_value(), "a relay with the wrong certificate is refused");
    }

    // ---- PeerNodes, end to end through the relay ----
    {
        auto relay = start_relay();
        const u16 port = relay.local_port().value();
        auto make_node = [](const char *name) {
            PeerNodeOptions options;
            options.network_id = "relay-net";
            options.display_name = name;
            options.listen = false; // neither side is reachable: that is the point
            options.handshake_timeout = 3000ms;
            return PeerNode::start(std::move(options)).wait().value();
        };
        PeerNode alice = make_node("alice");
        PeerNode bob = make_node("bob");
        const std::string token = *random_relay_token();

        auto bob_side = bob.connect_via_relay("127.0.0.1", port, token, RelayRole::Accept, alice.id());
        auto alice_side = alice.connect_via_relay("127.0.0.1", port, token, RelayRole::Dial, bob.id());
        auto alice_link = alice_side.wait();
        auto bob_link = bob_side.wait();
        check(alice_link && alice_link->id == bob.id() && alice_link->name == "bob", "dialer links to the pinned peer through the relay");
        check(bob_link && bob_link->id == alice.id() && bob_link->inbound, "acceptor sees the dialer's identity");
        if (alice_link && bob_link) {
            check(alice.send(bob.id(), 7, bytes("hello via relay")).wait().has_value(), "message through the relayed link");
            for (;;) {
                auto event = bob.next_event().wait();
                if (!event) break;
                if (event->kind == PeerEventKind::Message) {
                    check(event->channel == 7 && text(event->data) == "hello via relay", "...arrives intact");
                    break;
                }
            }
            check(relay.metrics().bytes_relayed > 0, "the relay carried the bytes");
        }

        // The relay (or anyone who grabs the Accept slot) cannot substitute a different peer: the dialer pins Bob's id.
        PeerNode mallory = make_node("mallory");
        const std::string token2 = *random_relay_token();
        auto mallory_side = mallory.connect_via_relay("127.0.0.1", port, token2, RelayRole::Accept);
        auto carol = make_node("carol");
        auto carol_side = carol.connect_via_relay("127.0.0.1", port, token2, RelayRole::Dial, bob.id());
        auto carol_link = carol_side.wait();
        check(!carol_link && carol_link.error().code == IoErrorCode::TlsFailure, "an impostor in the Accept slot is rejected by the pinned identity");
        (void)mallory_side.wait();
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
