// PeerNode (identity, links, rules, exchange, limits, discovery) and the STUN client.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <chrono>
#include <cstring>
#include <functional>
#include <optional>
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
            Foundation::log_error("P2pTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> bytes(std::string_view text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }
    std::string text(const std::vector<std::byte> &data) { return std::string(reinterpret_cast<const char *>(data.data()), data.size()); }

    PeerNodeOptions options_for(const std::string &network, const std::string &name) {
        PeerNodeOptions options;
        options.network_id = network;
        options.display_name = name;
        options.bind_address = "127.0.0.1";
        options.handshake_timeout = 3000ms;
        return options;
    }

    PeerNode start(PeerNodeOptions options) { return PeerNode::start(std::move(options)).wait().value(); }

    /// Waits up to `limit` for an event satisfying `predicate`, skipping the rest.
    std::optional<PeerEvent> expect_event(PeerNode &node, const std::function<bool(const PeerEvent &)> &predicate, std::chrono::milliseconds limit = 5000ms) {
        CancellationSource stop;
        auto timer_source = std::make_shared<CancellationSource>();
        auto timer = sleep_for(limit, timer_source->token());
        timer.on_done([timer, &stop]() mutable {
            try {
                timer.wait();
                (void)stop.cancel();
            } catch (...) {
            }
        });
        std::optional<PeerEvent> found;
        for (;;) {
            try {
                auto event = node.next_event(stop.token()).wait();
                if (!event) break;
                if (predicate(*event)) {
                    found = std::move(*event);
                    break;
                }
            } catch (const IoOperationCancelled &) {
                break;
            }
        }
        (void)timer_source->cancel();
        std::this_thread::sleep_for(5ms); // let the timer continuation finish before `stop` goes out of scope
        return found;
    }

    bool wait_until(const std::function<bool()> &condition, std::chrono::milliseconds limit = 5000ms) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return condition();
    }

    void basic_tests() {
        auto a = start(options_for("net-1", "alice"));
        auto b = start(options_for("net-1", "bob"));
        check(a.id().size() == 64 && b.id().size() == 64 && a.id() != b.id(), "nodes have distinct 64-char ids");
        check(a.id() == a.identity().sha256, "the id is the certificate fingerprint");

        // Dial with the expected id (pinned).
        auto linked = b.connect("127.0.0.1", a.listen_port().value(), a.id()).wait();
        check(linked.has_value() && linked->id == a.id() && linked->name == "alice", "connect pins the expected peer");
        auto a_connected = expect_event(a, [](const PeerEvent &e) { return e.kind == PeerEventKind::Connected; });
        check(a_connected && a_connected->peer.id == b.id() && a_connected->peer.inbound && a_connected->peer.name == "bob", "listener sees the dialer's identity");
        auto b_connected = expect_event(b, [](const PeerEvent &e) { return e.kind == PeerEventKind::Connected; });
        check(b_connected && !b_connected->peer.inbound, "dialer sees an outbound link");

        // Messages, both directions, per-channel, ordered.
        for (int i = 0; i < 20; ++i) (void)b.send(a.id(), 7, bytes("m" + std::to_string(i)));
        bool in_order = true;
        for (int i = 0; i < 20; ++i) {
            auto m = expect_event(a, [](const PeerEvent &e) { return e.kind == PeerEventKind::Message; });
            in_order = in_order && m && m->channel == 7 && m->peer.id == b.id() && text(m->data) == "m" + std::to_string(i);
        }
        check(in_order, "20 messages arrive in order with sender identity");
        (void)a.send(b.id(), 3, bytes("pong")).wait();
        auto back = expect_event(b, [](const PeerEvent &e) { return e.kind == PeerEventKind::Message; });
        check(back && back->channel == 3 && text(back->data) == "pong", "reverse direction works");

        std::vector<std::byte> big(900 * 1024, std::byte{0x5A});
        (void)a.send(b.id(), 1, big).wait();
        auto big_in = expect_event(b, [](const PeerEvent &e) { return e.kind == PeerEventKind::Message; });
        check(big_in && big_in->data == big, "900 KiB message arrives intact");

        check(a.broadcast(2, bytes("all")).wait().value() == 1, "broadcast reaches the connected peer");
        check(a.send(b.id(), 0xFF00, bytes("x")).wait().error().code == IoErrorCode::InvalidArgument, "reserved channels are refused");
        check(a.send(std::string(64, 'a'), 1, bytes("x")).wait().error().code == IoErrorCode::NotFound, "sending to an unknown peer fails");
        check(a.peers().size() == 1 && b.peers().size() == 1, "both sides list one peer");

        // Re-dialing an existing peer returns the existing link instead of making another.
        auto again = b.connect("127.0.0.1", a.listen_port().value(), a.id()).wait();
        check(again.has_value() && b.peers().size() == 1, "dialing a connected peer is idempotent");

        // Disconnect by the application is seen by the other side.
        b.disconnect(a.id());
        auto gone = expect_event(a, [](const PeerEvent &e) { return e.kind == PeerEventKind::Disconnected; });
        check(gone && gone->peer.id == b.id(), "disconnect produces a Disconnected event on the far side");
        check(wait_until([&] { return a.peers().empty() && b.peers().empty(); }), "peer tables empty after disconnect");

        // Stopping ends the event stream.
        a.stop();
        check(a.next_event().wait().error().code == IoErrorCode::Closed, "next_event after stop reports Closed");
        b.stop();
    }

    void rules_tests() {
        auto a = start(options_for("net-1", "alice"));
        auto b = start(options_for("net-1", "bob"));
        auto stranger = start(options_for("other-net", "eve"));

        // A pin mismatch is refused during TLS: we dial a's address but demand some other identity.
        auto wrong = b.connect("127.0.0.1", a.listen_port().value(), std::string(64, 'f')).wait();
        check(!wrong.has_value() && wrong.error().code == IoErrorCode::TlsFailure, "wrong expected id is refused");

        // Different network ids never link.
        auto cross = stranger.connect("127.0.0.1", a.listen_port().value(), a.id()).wait();
        check(!cross.has_value(), "peers of another network are refused");
        check(a.peers().empty(), "the other-network peer was not registered");

        // Block list.
        a.block(b.id());
        auto blocked = b.connect("127.0.0.1", a.listen_port().value(), a.id()).wait();
        check(!blocked.has_value() || !wait_until([&] { return !a.peers().empty(); }, 300ms), "a blocked peer cannot stay connected");
        check(a.peers().empty(), "blocked peer is not in the table");

        // Allow list.
        auto options = options_for("net-1", "gate");
        options.allowed_peers = {b.id()};
        auto gate = start(options);
        auto stranger_same_net = start(options_for("net-1", "mallory"));
        auto denied = stranger_same_net.connect("127.0.0.1", gate.listen_port().value(), gate.id()).wait();
        check(!denied.has_value() || !wait_until([&] { return !gate.peers().empty(); }, 300ms), "a peer outside the allow list cannot connect");
        auto permitted = b.connect("127.0.0.1", gate.listen_port().value(), gate.id()).wait();
        check(permitted.has_value(), "an allow-listed peer connects");

        // Oversized frames close the link on the receiver and are reported to the sender.
        auto strict = options_for("net-1", "strict");
        strict.max_message_bytes = 1000;
        auto strict_node = start(strict);
        auto sender = start(options_for("net-1", "sender"));
        check(sender.connect("127.0.0.1", strict_node.listen_port().value(), strict_node.id()).wait().has_value(), "link to the strict node");
        (void)sender.send(strict_node.id(), 5, std::vector<std::byte>(5000)).wait();
        auto cut = expect_event(sender, [](const PeerEvent &e) { return e.kind == PeerEventKind::Disconnected; });
        check(cut.has_value(), "a frame over the receiver's limit drops the link");

        // Local limit check happens before sending.
        auto tiny_local = options_for("net-1", "tiny");
        tiny_local.max_message_bytes = 10;
        auto tiny = start(tiny_local);
        check(tiny.send(a.id(), 1, std::vector<std::byte>(11)).wait().error().code == IoErrorCode::LimitExceeded, "local send limit");

        // Invalid configuration is rejected.
        auto bad = options_for("", "x");
        check(!PeerNode::start(bad).wait().has_value(), "empty network id is rejected");
    }

    void simultaneous_dial_test() {
        auto a = start(options_for("net-1", "alice"));
        auto b = start(options_for("net-1", "bob"));
        auto ab = b.connect("127.0.0.1", a.listen_port().value(), a.id());
        auto ba = a.connect("127.0.0.1", b.listen_port().value(), b.id());
        (void)ab.wait();
        (void)ba.wait();
        check(wait_until([&] { return a.peers().size() == 1 && b.peers().size() == 1; }), "crossed dials settle on exactly one link per side");
        std::this_thread::sleep_for(150ms);
        check(a.peers().size() == 1 && b.peers().size() == 1, "the single link stays up");
        // And the surviving link works.
        (void)a.send(b.id(), 9, bytes("still works")).wait();
        auto got = expect_event(b, [](const PeerEvent &e) { return e.kind == PeerEventKind::Message; });
        check(got && text(got->data) == "still works", "the surviving link carries traffic");
    }

    void exchange_test() {
        auto a = start(options_for("net-x", "a"));
        auto b = start(options_for("net-x", "b"));
        auto c = start(options_for("net-x", "c"));
        check(b.connect("127.0.0.1", a.listen_port().value(), a.id()).wait().has_value(), "b -> a");
        check(wait_until([&] { return a.peers().size() == 1; }), "a sees b");
        check(c.connect("127.0.0.1", a.listen_port().value(), a.id()).wait().has_value(), "c -> a");
        // a tells c about b (and b about c): the mesh fills in without anyone dialing b or c by hand.
        check(wait_until([&] { return b.peers().size() == 2 && c.peers().size() == 2; }, 6000ms), "peer exchange completes the mesh");
    }

    void liveness_test() {
        auto fast = options_for("net-1", "fast");
        fast.ping_interval = 100ms;
        fast.idle_timeout = 500ms;
        auto a = start(fast);
        auto b = start(fast);
        check(b.connect("127.0.0.1", a.listen_port().value(), a.id()).wait().has_value(), "link");
        std::this_thread::sleep_for(1500ms); // three idle timeouts: only pings/pongs keep it up
        check(a.peers().size() == 1 && b.peers().size() == 1, "idle links survive on keep-alive pings");
    }

    void discovery_test() {
        auto base = options_for("lan-net", "x");
        base.bind_address.clear(); // announcements arrive from the LAN address, so the node must listen on all interfaces
        base.lan_discovery = true;
        base.discovery_port = 47932;
        base.discovery_interval = 200ms;
        auto a = start(base);
        auto b = start(base);
        auto found = expect_event(a, [&](const PeerEvent &e) { return e.kind == PeerEventKind::Discovered && e.peer.id == b.id(); }, 4000ms);
        if (!found) {
            Foundation::log_info("P2pTest: no LAN announcements delivered here (no broadcast/multicast route); discovery check skipped");
            return;
        }
        check(wait_until([&] { return a.peers().size() == 1 && b.peers().size() == 1; }), "discovered peers connect automatically");
    }

    // ---- STUN against a fake server --------------------------------------------------------------
    TaskHandle<void> fake_stun(UdpSocket &server, bool answer) {
        for (;;) {
            auto request = co_await server.receive_from(1500);
            if (!request) co_return;
            if (!answer || request->data.size() < 20) continue;
            // A stray packet first: the client must ignore anything that is not its transaction.
            (void)co_await server.send_to(request->sender_host, request->sender_port, bytes("garbage-not-stun-garbage"));
            std::vector<std::byte> reply(32, std::byte{0});
            reply[0] = std::byte{0x01};
            reply[1] = std::byte{0x01};   // Binding success
            reply[3] = std::byte{12};     // body length
            for (int i = 4; i < 20; ++i) reply[static_cast<std::size_t>(i)] = request->data[static_cast<std::size_t>(i)]; // cookie + transaction id
            reply[20] = std::byte{0x00};
            reply[21] = std::byte{0x20};  // XOR-MAPPED-ADDRESS
            reply[23] = std::byte{8};
            reply[25] = std::byte{0x01};  // IPv4
            const std::uint16_t port = 4242 ^ 0x2112;
            reply[26] = static_cast<std::byte>(port >> 8);
            reply[27] = static_cast<std::byte>(port & 0xFF);
            const unsigned char address[4] = {203, 0, 113, 9};
            const unsigned char cookie[4] = {0x21, 0x12, 0xA4, 0x42};
            for (int i = 0; i < 4; ++i) reply[static_cast<std::size_t>(28 + i)] = static_cast<std::byte>(address[i] ^ cookie[i]);
            (void)co_await server.send_to(request->sender_host, request->sender_port, reply);
        }
    }

    void stun_tests() {
        auto server = UdpSocket::bind("127.0.0.1", 0).wait().value();
        const u16 port = server.local_port().value();
        auto loop = fake_stun(server, true);
        auto mapped = stun_query("127.0.0.1", port, 2000ms).wait();
        check(mapped.has_value() && mapped->host == "203.0.113.9" && mapped->port == 4242, "STUN XOR-MAPPED-ADDRESS decoded");
        server.close();
        loop.wait();

        auto silent = UdpSocket::bind("127.0.0.1", 0).wait().value();
        auto silent_loop = fake_stun(silent, false);
        const auto start_time = std::chrono::steady_clock::now();
        auto timed_out = stun_query("127.0.0.1", silent.local_port().value(), 800ms).wait();
        check(!timed_out.has_value() && timed_out.error().code == IoErrorCode::TimedOut, "an unresponsive STUN server times out");
        check(std::chrono::steady_clock::now() - start_time < 3s, "the timeout is honoured");
        silent.close();
        silent_loop.wait();
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    basic_tests();
    rules_tests();
    simultaneous_dial_test();
    exchange_test();
    liveness_test();
    discovery_test();
    stun_tests();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
