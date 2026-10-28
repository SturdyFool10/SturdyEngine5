#include <Async/P2p.hpp>

#include <Async/Channel.hpp>
#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Timer.hpp>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <map>
#include <set>

namespace SFT::Async {

    namespace Detail {

        using Bytes = std::vector<std::byte>;
        template <typename T>
        using Result = std::expected<T, IoError>;

        constexpr std::uint16_t kControlBase = 0xFF00;
        constexpr std::uint16_t kHello = 0xFFFF;
        constexpr std::uint16_t kPing = 0xFFFE;
        constexpr std::uint16_t kPong = 0xFFFD;
        constexpr std::uint16_t kPeers = 0xFFFC;
        constexpr std::uint16_t kProtocolVersion = 1;
        constexpr std::size_t kMaxPeerListEntries = 16;
        constexpr std::size_t kIdLength = 64;

        template <typename T>
        TaskHandle<Result<T>> failed(IoError error) {
            Promise<Result<T>> promise;
            (void)promise.set_value(Result<T>(std::unexpected(std::move(error))));
            return promise.handle();
        }

        std::int64_t now_ms() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        }

        std::string lowered(std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // ---- tiny bounds-checked binary codec ----------------------------------------------------
        struct Writer {
            Bytes bytes;
            void u8(std::uint8_t v) { bytes.push_back(static_cast<std::byte>(v)); }
            void u16(std::uint16_t v) {
                u8(static_cast<std::uint8_t>(v >> 8));
                u8(static_cast<std::uint8_t>(v));
            }
            void u32(std::uint32_t v) {
                u16(static_cast<std::uint16_t>(v >> 16));
                u16(static_cast<std::uint16_t>(v));
            }
            void str8(const std::string &text) {
                const std::size_t size = (std::min)(text.size(), std::size_t{255});
                u8(static_cast<std::uint8_t>(size));
                for (std::size_t i = 0; i < size; ++i) u8(static_cast<std::uint8_t>(text[i]));
            }
        };

        struct Reader {
            const std::byte *data;
            std::size_t size;
            std::size_t at = 0;
            bool ok = true;
            std::uint8_t u8() {
                if (at >= size) {
                    ok = false;
                    return 0;
                }
                return std::to_integer<std::uint8_t>(data[at++]);
            }
            std::uint16_t u16() {
                const unsigned high = u8();
                return static_cast<std::uint16_t>((high << 8) | u8());
            }
            std::string str8() {
                const std::size_t length = u8();
                if (!ok || at + length > size) {
                    ok = false;
                    return {};
                }
                std::string out(reinterpret_cast<const char *>(data + at), length);
                at += length;
                return out;
            }
        };

        Bytes make_frame(std::uint16_t channel, const Bytes &payload) {
            Writer w;
            w.u32(static_cast<std::uint32_t>(payload.size() + 2));
            w.u16(channel);
            w.bytes.insert(w.bytes.end(), payload.begin(), payload.end());
            return std::move(w.bytes);
        }

        struct Frame {
            std::uint16_t channel = 0;
            Bytes payload;
        };

        // ---- node state --------------------------------------------------------------------------
        struct PeerLink {
            PeerInfo info;
            TcpConnection conn;
            std::atomic<std::int64_t> last_seen_ms{0};
        };

        struct PeerState {
            std::map<PeerId, std::shared_ptr<PeerLink>> links;
            std::set<std::string> dialing;
            std::set<PeerId> blocked;
            std::set<PeerId> allowed;
            std::map<std::string, std::int64_t> candidates_seen;
        };

        struct PeerCore : std::enable_shared_from_this<PeerCore> {
            PeerCore(PeerNodeOptions o, TlsIdentity i, PeerId peer_id, ChannelPair<PeerEvent> events)
                : options(std::move(o)), identity(std::move(i)), id(std::move(peer_id)), events_tx(std::move(events.sender)),
                  events_rx(std::move(events.receiver)) {
                auto guard = state.lock();
                for (const auto &allowed : options.allowed_peers) guard->allowed.insert(lowered(allowed));
            }

            PeerNodeOptions options;
            TlsIdentity identity;
            PeerId id;
            TcpListener listener;
            UdpSocket discovery;
            Sender<PeerEvent> events_tx;
            Receiver<PeerEvent> events_rx;
            Mutex<PeerState> state;
            std::atomic<bool> stopped{false};
            std::atomic<std::size_t> pending_inbound{0};
            CancellationSource stop_source;
            TlsClientOptions client_tls;
            TlsServerOptions server_tls; ///< also used for links that arrive through a relay
            std::uint16_t advertised_port = 0;

            [[nodiscard]] bool permitted(const PeerId &peer) {
                auto guard = state.lock();
                if (guard->blocked.contains(peer)) return false;
                return guard->allowed.empty() || guard->allowed.contains(peer);
            }
        };

        TaskHandle<Result<Frame>> read_frame(TcpConnection *connection, std::size_t max_payload, CancellationToken token) {
            auto header = co_await connection->receive_exact(6, token);
            if (!header) co_return std::unexpected(header.error());
            Reader r{header->data(), header->size()};
            const std::uint32_t high = r.u16();
            const std::uint32_t length = (high << 16) | r.u16();
            const std::uint16_t channel = r.u16();
            if (length < 2 || length - 2 > max_payload) {
                co_return std::unexpected(IoError{IoErrorCode::LimitExceeded, "peer frame exceeds the message size limit"});
            }
            Frame frame;
            frame.channel = channel;
            if (length > 2) {
                auto body = co_await connection->receive_exact(length - 2, token);
                if (!body) co_return std::unexpected(body.error());
                frame.payload = std::move(*body);
            }
            co_return Result<Frame>(std::move(frame));
        }

        /// Queues an event for the application. Waits when the queue is full (back-pressure); false once the node stopped.
        TaskHandle<bool> emit(std::shared_ptr<PeerCore> core, PeerEvent event) {
            bool delivered = true;
            try {
                co_await core->events_tx.send(std::move(event));
            } catch (const ChannelClosed &) {
                delivered = false;
            }
            co_return delivered;
        }

        void maybe_dial(const std::shared_ptr<PeerCore> &core, const std::string &host, std::uint16_t port, const PeerId &expected);
        TaskHandle<Result<PeerInfo>> dial(std::shared_ptr<PeerCore> core, std::string host, std::uint16_t port, std::optional<PeerId> expected);

        TaskHandle<void> drop_link(std::shared_ptr<PeerCore> core, std::shared_ptr<PeerLink> link, std::string reason) {
            bool registered = false;
            {
                auto guard = core->state.lock();
                auto it = guard->links.find(link->info.id);
                if (it != guard->links.end() && it->second == link) {
                    guard->links.erase(it);
                    registered = true;
                }
            }
            link->conn.close();
            if (registered && !core->stopped.load(std::memory_order_acquire)) {
                PeerEvent event;
                event.kind = PeerEventKind::Disconnected;
                event.peer = link->info;
                event.reason = std::move(reason);
                (void)co_await emit(core, std::move(event));
            }
        }

        Bytes peer_list_for(const std::shared_ptr<PeerCore> &core, const PeerId &except) {
            Writer w;
            std::vector<std::shared_ptr<PeerLink>> chosen;
            {
                auto guard = core->state.lock();
                for (const auto &[peer_id, link] : guard->links) {
                    if (peer_id != except && link->info.listen_port != 0 && chosen.size() < kMaxPeerListEntries) chosen.push_back(link);
                }
            }
            w.u8(static_cast<std::uint8_t>(chosen.size()));
            for (const auto &link : chosen) {
                w.str8(link->info.address.host);
                w.u16(link->info.listen_port);
                w.str8(link->info.id);
            }
            return std::move(w.bytes);
        }

        TaskHandle<void> handle_peer_list(std::shared_ptr<PeerCore> core, std::shared_ptr<PeerLink> from, Bytes payload) {
            if (!core->options.peer_exchange) co_return;
            Reader r{payload.data(), payload.size()};
            const unsigned count = (std::min)(static_cast<unsigned>(r.u8()), static_cast<unsigned>(kMaxPeerListEntries));
            for (unsigned i = 0; i < count && r.ok; ++i) {
                std::string host = r.str8();
                const std::uint16_t port = r.u16();
                PeerId peer = lowered(r.str8());
                if (!r.ok || peer.size() != kIdLength || port == 0 || host.empty() || peer == core->id) continue;
                bool fresh = false;
                {
                    auto guard = core->state.lock();
                    if (guard->links.contains(peer) || guard->blocked.contains(peer)) continue;
                    const auto now = now_ms();
                    auto [it, inserted] = guard->candidates_seen.try_emplace(peer, now);
                    fresh = inserted || now - it->second > 30'000;
                    it->second = now;
                    if (guard->candidates_seen.size() > 512) guard->candidates_seen.clear();
                }
                if (!fresh) continue;
                PeerEvent event;
                event.kind = PeerEventKind::Discovered;
                event.peer.id = peer;
                event.peer.address = {host, port};
                event.peer.listen_port = port;
                (void)co_await emit(core, std::move(event));
                maybe_dial(core, host, port, peer);
            }
            (void)from;
        }

        TaskHandle<void> peer_reader(std::shared_ptr<PeerCore> core, std::shared_ptr<PeerLink> link) {
            std::string reason = "connection closed";
            try {
                for (;;) {
                    auto frame = co_await read_frame(&link->conn, core->options.max_message_bytes, core->stop_source.token());
                    if (!frame) {
                        reason = frame.error().message;
                        break;
                    }
                    link->last_seen_ms.store(now_ms(), std::memory_order_release);
                    if (frame->channel >= kControlBase) {
                        if (frame->channel == kPing) {
                            (void)link->conn.send_all(make_frame(kPong, {}));
                        } else if (frame->channel == kPeers) {
                            (void)handle_peer_list(core, link, std::move(frame->payload));
                        } else if (frame->channel == kHello) {
                            reason = "protocol violation: repeated hello";
                            break;
                        }
                        continue;
                    }
                    PeerEvent event;
                    event.kind = PeerEventKind::Message;
                    event.peer = link->info;
                    event.channel = frame->channel;
                    event.data = std::move(frame->payload);
                    // Waiting here when the application is slow is the back-pressure: we stop reading this peer.
                    auto delivered = co_await emit(core, std::move(event));
                    if (!delivered) {
                        reason = "node stopped";
                        break;
                    }
                }
            } catch (...) {
                reason = "node stopped";
            }
            co_await drop_link(core, link, reason);
        }

        TaskHandle<void> peer_keepalive(std::shared_ptr<PeerCore> core, std::shared_ptr<PeerLink> link) {
            Interval ticker(core->options.ping_interval);
            try {
                for (;;) {
                    co_await ticker.tick(core->stop_source.token());
                    if (core->stopped.load(std::memory_order_acquire) || !link->conn.is_open()) break;
                    const auto idle = now_ms() - link->last_seen_ms.load(std::memory_order_acquire);
                    if (idle > core->options.idle_timeout.count()) {
                        link->conn.close(); // the reader notices and reports "connection closed"
                        break;
                    }
                    (void)link->conn.send_all(make_frame(kPing, {}));
                }
            } catch (...) {
            }
        }

        Bytes hello_payload(const PeerCore &core) {
            Writer w;
            w.u16(kProtocolVersion);
            w.str8(core.options.network_id);
            w.u16(core.advertised_port);
            w.str8(core.options.display_name);
            return std::move(w.bytes);
        }

        /// Runs after TLS: proves who the peer is (certificate fingerprint), exchanges hellos under a deadline,
        /// resolves duplicate links, registers the peer and starts its service loops.
        TaskHandle<Result<PeerInfo>> establish(std::shared_ptr<PeerCore> core, TcpConnection connection, bool inbound,
                                               std::optional<PeerId> expected) {
            auto fail = [&](IoErrorCode code, std::string message) {
                connection.close();
                return Result<PeerInfo>(std::unexpected(IoError{code, std::move(message)}));
            };
            if (core->stopped.load(std::memory_order_acquire)) co_return fail(IoErrorCode::Closed, "node stopped");

            auto tls = connection.tls_info();
            if (!tls || tls->peer_sha256.size() != kIdLength) co_return fail(IoErrorCode::TlsFailure, "peer presented no certificate");
            const PeerId peer_id = tls->peer_sha256;
            if (expected && lowered(*expected) != peer_id) co_return fail(IoErrorCode::TlsFailure, "peer identity does not match the expected id");
            if (peer_id == core->id) co_return fail(IoErrorCode::InvalidArgument, "refusing to connect to ourselves");
            if (!core->permitted(peer_id)) co_return fail(IoErrorCode::PermissionDenied, "peer is blocked or not on the allow list");

            // Hello exchange under one deadline.
            auto deadline = std::make_shared<CancellationSource>();
            auto timer_stop = std::make_shared<CancellationSource>();
            {
                auto timer = sleep_for(core->options.handshake_timeout, timer_stop->token());
                timer.on_done([timer, deadline]() mutable {
                    try {
                        timer.wait();
                        (void)deadline->cancel();
                    } catch (...) {
                    }
                });
            }
            Result<Frame> hello = std::unexpected(IoError{IoErrorCode::TimedOut, "peer handshake timed out"});
            try {
                auto sent = co_await connection.send_all(make_frame(kHello, hello_payload(*core)), deadline->token());
                if (sent) hello = co_await read_frame(&connection, 1024, deadline->token());
                else hello = std::unexpected(sent.error());
            } catch (const IoOperationCancelled &) {
            }
            (void)timer_stop->cancel();
            if (!hello) co_return fail(hello.error().code, "peer handshake failed: " + hello.error().message);
            if (hello->channel != kHello) co_return fail(IoErrorCode::ProtocolError, "peer did not start with a hello");

            Reader r{hello->payload.data(), hello->payload.size()};
            const std::uint16_t version = r.u16();
            const std::string network = r.str8();
            const std::uint16_t listen_port = r.u16();
            const std::string name = r.str8();
            if (!r.ok || version != kProtocolVersion) co_return fail(IoErrorCode::ProtocolError, "unsupported peer protocol version");
            if (network != core->options.network_id) co_return fail(IoErrorCode::PermissionDenied, "peer belongs to a different network");

            auto link = std::make_shared<PeerLink>();
            link->info.id = peer_id;
            link->info.address = connection.peer_address().value_or(SocketAddress{});
            link->info.name = name;
            link->info.inbound = inbound;
            link->info.listen_port = listen_port;
            link->last_seen_ms.store(now_ms());
            link->conn = std::move(connection);

            std::shared_ptr<PeerLink> replaced;
            std::optional<PeerInfo> existing;
            {
                auto guard = core->state.lock();
                if (core->stopped.load(std::memory_order_acquire)) {
                    link->conn.close();
                    co_return Result<PeerInfo>(std::unexpected(IoError{IoErrorCode::Closed, "node stopped"}));
                }
                auto it = guard->links.find(peer_id);
                if (it != guard->links.end()) {
                    // Both ends dialed each other: keep the link started by the node with the smaller id, on both sides.
                    const PeerId preferred = (std::min)(core->id, peer_id);
                    const PeerId new_initiator = inbound ? peer_id : core->id;
                    const PeerId old_initiator = it->second->info.inbound ? peer_id : core->id;
                    if (new_initiator == preferred && old_initiator != preferred) {
                        replaced = it->second;
                        it->second = link;
                    } else {
                        existing = it->second->info;
                    }
                } else if (guard->links.size() >= core->options.max_peers) {
                    link->conn.close();
                    co_return Result<PeerInfo>(std::unexpected(IoError{IoErrorCode::LimitExceeded, "peer limit reached"}));
                } else {
                    guard->links[peer_id] = link;
                }
            }
            if (existing) {
                link->conn.close();
                co_return Result<PeerInfo>(*existing);
            }
            if (replaced) replaced->conn.close(); // its reader sees the close, finds it no longer registered, and stays silent

            PeerEvent connected;
            connected.kind = PeerEventKind::Connected;
            connected.peer = link->info;
            (void)co_await emit(core, std::move(connected));
            (void)peer_reader(core, link);
            (void)peer_keepalive(core, link);
            if (core->options.peer_exchange) (void)link->conn.send_all(make_frame(kPeers, peer_list_for(core, peer_id)));
            co_return Result<PeerInfo>(link->info);
        }

        TaskHandle<Result<PeerInfo>> dial(std::shared_ptr<PeerCore> core, std::string host, std::uint16_t port, std::optional<PeerId> expected) {
            if (core->stopped.load(std::memory_order_acquire)) co_return std::unexpected(IoError{IoErrorCode::Closed, "node stopped"});
            if (expected) {
                expected = lowered(*expected);
                std::shared_ptr<PeerLink> link;
                {
                    auto guard = core->state.lock();
                    if (auto it = guard->links.find(*expected); it != guard->links.end()) link = it->second;
                }
                if (link) co_return Result<PeerInfo>(link->info);
                if (!core->permitted(*expected)) co_return std::unexpected(IoError{IoErrorCode::PermissionDenied, "peer is blocked or not on the allow list"});
            }
            const std::string key = host + ":" + std::to_string(port);
            {
                auto guard = core->state.lock();
                guard->dialing.insert(key);
            }
            TlsClientOptions tls = core->client_tls;
            if (expected) tls.pinned_sha256 = {*expected};
            else tls.identify_peer_by_certificate_only = true;
            Result<TcpConnection> connection = std::unexpected(IoError{IoErrorCode::Closed, "node stopped"});
            try {
                connection = co_await TcpConnection::connect_tls(host, port, tls, core->options.handshake_timeout, core->stop_source.token());
            } catch (const IoOperationCancelled &) {
            }
            {
                auto guard = core->state.lock();
                guard->dialing.erase(key);
            }
            if (!connection) co_return std::unexpected(connection.error());
            co_return co_await establish(core, std::move(*connection), false, expected);
        }

        /// The relay hands over a plain pipe to the partner; the peer-to-peer TLS then runs through it, end to end.
        TaskHandle<Result<PeerInfo>> meet_via_relay(std::shared_ptr<PeerCore> core, std::string host, std::uint16_t port, std::string token, RelayRole role,
                                                    std::optional<PeerId> expected, std::optional<TlsClientOptions> relay_tls) {
            if (core->stopped.load(std::memory_order_acquire)) co_return std::unexpected(IoError{IoErrorCode::Closed, "node stopped"});
            if (expected) {
                expected = lowered(*expected);
                if (!core->permitted(*expected)) co_return std::unexpected(IoError{IoErrorCode::PermissionDenied, "peer is blocked or not on the allow list"});
            }
            Result<TcpConnection> piped = std::unexpected(IoError{IoErrorCode::Closed, "node stopped"});
            try {
                piped = co_await relay_connect(std::move(host), port, std::move(token), role, std::move(relay_tls), std::chrono::milliseconds(35'000),
                                               core->stop_source.token());
            } catch (const IoOperationCancelled &) {
            }
            if (!piped) co_return std::unexpected(piped.error());
            Result<void> secured;
            if (role == RelayRole::Dial) {
                TlsClientOptions tls = core->client_tls;
                if (expected) tls.pinned_sha256 = {*expected};
                else tls.identify_peer_by_certificate_only = true;
                secured = co_await piped->start_tls_client("relayed-peer", tls, core->stop_source.token());
            } else {
                secured = co_await piped->start_tls_server(core->server_tls);
            }
            if (!secured) {
                piped->close();
                co_return std::unexpected(secured.error());
            }
            co_return co_await establish(core, std::move(*piped), role == RelayRole::Accept, expected);
        }

        void maybe_dial(const std::shared_ptr<PeerCore> &core, const std::string &host, std::uint16_t port, const PeerId &expected) {
            if (!core->options.auto_connect || core->stopped.load(std::memory_order_acquire)) return;
            const std::string key = host + ":" + std::to_string(port);
            {
                auto guard = core->state.lock();
                if (guard->links.size() + guard->dialing.size() >= core->options.max_peers) return;
                if (guard->links.contains(expected) || guard->dialing.contains(key) || guard->blocked.contains(expected)) return;
                if (!guard->allowed.empty() && !guard->allowed.contains(expected)) return;
            }
            (void)dial(core, host, port, expected);
        }

        TaskHandle<void> inbound_session(std::shared_ptr<PeerCore> core, TcpConnection connection) {
            (void)co_await establish(core, std::move(connection), true, std::nullopt);
            core->pending_inbound.fetch_sub(1, std::memory_order_acq_rel);
        }

        TaskHandle<void> accept_loop(std::shared_ptr<PeerCore> core) {
            try {
                for (;;) {
                    auto accepted = co_await core->listener.accept(core->stop_source.token());
                    if (core->stopped.load(std::memory_order_acquire)) break;
                    if (!accepted) {
                        if (accepted.error().code == IoErrorCode::Closed) break;
                        continue;
                    }
                    if (core->pending_inbound.load(std::memory_order_acquire) >= core->options.max_pending_inbound) {
                        accepted->close();
                        continue;
                    }
                    core->pending_inbound.fetch_add(1, std::memory_order_acq_rel);
                    (void)inbound_session(core, std::move(*accepted));
                }
            } catch (...) {
            }
        }

        // ---- LAN discovery -----------------------------------------------------------------------
        Bytes announcement(const PeerCore &core) {
            Writer w;
            for (char c : std::string("SPN1")) w.u8(static_cast<std::uint8_t>(c));
            w.str8(core.options.network_id);
            w.str8(core.id);
            w.u16(core.advertised_port);
            w.str8(core.options.display_name);
            return std::move(w.bytes);
        }

        TaskHandle<void> discovery_announcer(std::shared_ptr<PeerCore> core) {
            Interval ticker(core->options.discovery_interval);
            try {
                for (;;) {
                    if (core->advertised_port != 0) {
                        const Bytes packet = announcement(*core);
                        (void)core->discovery.send_to(core->options.discovery_group, core->options.discovery_port, packet);
                        (void)core->discovery.send_to("255.255.255.255", core->options.discovery_port, packet);
                    }
                    co_await ticker.tick(core->stop_source.token());
                    if (core->stopped.load(std::memory_order_acquire)) break;
                }
            } catch (...) {
            }
        }

        TaskHandle<void> discovery_listener(std::shared_ptr<PeerCore> core) {
            try {
                for (;;) {
                    auto datagram = co_await core->discovery.receive_from(1500, core->stop_source.token());
                    if (core->stopped.load(std::memory_order_acquire)) break;
                    if (!datagram) {
                        if (datagram.error().code == IoErrorCode::Closed) break;
                        continue;
                    }
                    Reader r{datagram->data.data(), datagram->data.size()};
                    std::string magic;
                    for (int i = 0; i < 4; ++i) magic.push_back(static_cast<char>(r.u8()));
                    const std::string network = r.str8();
                    const PeerId peer = lowered(r.str8());
                    const std::uint16_t port = r.u16();
                    const std::string name = r.str8();
                    if (!r.ok || magic != "SPN1" || network != core->options.network_id || peer.size() != kIdLength || peer == core->id || port == 0) continue;
                    bool fresh = false;
                    {
                        auto guard = core->state.lock();
                        if (guard->links.contains(peer) || guard->blocked.contains(peer)) continue;
                        const auto now = now_ms();
                        auto [it, inserted] = guard->candidates_seen.try_emplace(peer, now);
                        fresh = inserted || now - it->second > 30'000;
                        if (fresh) it->second = now;
                    }
                    if (!fresh) continue;
                    PeerEvent event;
                    event.kind = PeerEventKind::Discovered;
                    event.peer.id = peer;
                    event.peer.name = name;
                    event.peer.address = {datagram->sender_host, port};
                    event.peer.listen_port = port;
                    (void)co_await emit(core, std::move(event));
                    // An announcement is only a hint: the dial pins `peer`, so a forged one just fails the handshake.
                    maybe_dial(core, datagram->sender_host, port, peer);
                }
            } catch (...) {
            }
        }

    } // namespace Detail

    using namespace Detail;

    PeerNode::~PeerNode() noexcept { stop(); }
    PeerNode &PeerNode::operator=(PeerNode &&other) noexcept {
        if (this != &other) {
            stop();
            core_ = std::move(other.core_);
        }
        return *this;
    }

    TaskHandle<std::expected<PeerNode, IoError>> PeerNode::start(PeerNodeOptions options) {
        auto fail = [](IoErrorCode code, std::string message) {
            Promise<Result<PeerNode>> promise;
            (void)promise.set_value(Result<PeerNode>(std::unexpected(IoError{code, std::move(message)})));
            return promise.handle();
        };
        if (options.network_id.empty() || options.network_id.size() > 64) return fail(IoErrorCode::InvalidArgument, "network_id must be 1-64 bytes");
        if (options.display_name.size() > 64) return fail(IoErrorCode::InvalidArgument, "display_name is limited to 64 bytes");
        if (options.max_peers == 0 || options.max_message_bytes == 0 || options.event_queue == 0 || options.ping_interval.count() <= 0 ||
            options.idle_timeout <= options.ping_interval || options.handshake_timeout.count() <= 0) {
            return fail(IoErrorCode::InvalidArgument, "invalid peer node limits (idle_timeout must exceed ping_interval)");
        }
        TlsIdentity identity;
        if (options.identity) {
            identity = *options.identity;
        } else {
            auto generated = generate_self_signed_identity("sturdy-peer");
            if (!generated) return fail(generated.error().code, generated.error().message);
            identity = std::move(*generated);
        }
        auto fingerprint = certificate_sha256(identity.certificate_pem);
        if (!fingerprint) return fail(IoErrorCode::InvalidArgument, "identity.certificate_pem is not a certificate");
        identity.sha256 = *fingerprint;

        auto core = std::make_shared<PeerCore>(std::move(options), identity, *fingerprint, channel<PeerEvent>(options.event_queue));
        core->client_tls.client_certificate_chain_pem = identity.certificate_pem;
        core->client_tls.client_private_key_pem = identity.private_key_pem;
        core->client_tls.handshake_timeout_ms = static_cast<std::uint32_t>(core->options.handshake_timeout.count());

        {
            TlsServerOptions &tls = core->server_tls;
            tls.certificate_chain_pem = identity.certificate_pem;
            tls.private_key_pem = identity.private_key_pem;
            tls.require_client_certificate = true;
            tls.accept_any_client_certificate = true; // identity is the fingerprint; `permitted()` decides who may stay
            tls.handshake_timeout_ms = static_cast<std::uint32_t>(core->options.handshake_timeout.count());
        }
        if (core->options.listen) {
            auto listener = TcpListener::listen_tls(core->options.bind_address, core->options.port, core->server_tls).wait();
            if (!listener) return fail(listener.error().code, "peer node cannot listen: " + listener.error().message);
            core->listener = std::move(*listener);
            core->advertised_port = core->listener.local_port().value_or(0);
        }
        if (core->options.lan_discovery) {
            UdpOptions udp;
            udp.reuse_address = true;
            udp.broadcast = true;
            auto socket = UdpSocket::bind("0.0.0.0", core->options.discovery_port, udp).wait();
            if (!socket) return fail(socket.error().code, "LAN discovery cannot bind: " + socket.error().message);
            core->discovery = std::move(*socket);
            (void)core->discovery.join_multicast(core->options.discovery_group).wait(); // best effort; broadcast still works without it
            (void)core->discovery.set_multicast_ttl(1).wait();
        }

        PeerNode node;
        node.core_ = core;
        if (core->options.listen) (void)accept_loop(core);
        if (core->options.lan_discovery) {
            (void)discovery_listener(core);
            (void)discovery_announcer(core);
        }
        Promise<Result<PeerNode>> promise;
        (void)promise.set_value(Result<PeerNode>(std::move(node)));
        return promise.handle();
    }

    const PeerId &PeerNode::id() const noexcept {
        static const PeerId empty;
        return core_ ? core_->id : empty;
    }
    const TlsIdentity &PeerNode::identity() const noexcept {
        static const TlsIdentity empty;
        return core_ ? core_->identity : empty;
    }
    std::expected<std::uint16_t, IoError> PeerNode::listen_port() const {
        if (!core_) return std::unexpected(IoError{IoErrorCode::Closed, "node not started"});
        if (!core_->options.listen) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "node does not listen"});
        return core_->listener.local_port();
    }

    TaskHandle<std::expected<PeerInfo, IoError>> PeerNode::connect(std::string host, std::uint16_t port, std::optional<PeerId> expected) {
        if (!core_) return failed<PeerInfo>(IoError{IoErrorCode::Closed, "node not started"});
        return dial(core_, std::move(host), port, std::move(expected));
    }

    TaskHandle<std::expected<PeerInfo, IoError>> PeerNode::connect_via_relay(std::string relay_host, std::uint16_t relay_port, std::string token, RelayRole role,
                                                                             std::optional<PeerId> expected, std::optional<TlsClientOptions> relay_tls) {
        if (!core_) return failed<PeerInfo>(IoError{IoErrorCode::Closed, "node not started"});
        return meet_via_relay(core_, std::move(relay_host), relay_port, std::move(token), role, std::move(expected), std::move(relay_tls));
    }

    TaskHandle<std::expected<void, IoError>> PeerNode::send(const PeerId &peer, std::uint16_t channel, std::vector<std::byte> data) {
        if (!core_) return failed<void>(IoError{IoErrorCode::Closed, "node not started"});
        if (channel >= kControlBase) return failed<void>(IoError{IoErrorCode::InvalidArgument, "channels 0xFF00 and above are reserved"});
        if (data.size() > core_->options.max_message_bytes) return failed<void>(IoError{IoErrorCode::LimitExceeded, "message exceeds max_message_bytes"});
        std::shared_ptr<PeerLink> link;
        {
            auto guard = core_->state.lock();
            if (auto it = guard->links.find(lowered(peer)); it != guard->links.end()) link = it->second;
        }
        if (!link) return failed<void>(IoError{IoErrorCode::NotFound, "no such connected peer"});
        return link->conn.send_all(make_frame(channel, data));
    }

    TaskHandle<std::expected<std::size_t, IoError>> PeerNode::broadcast(std::uint16_t channel, std::vector<std::byte> data, std::optional<PeerId> except) {
        if (!core_) return failed<std::size_t>(IoError{IoErrorCode::Closed, "node not started"});
        if (channel >= kControlBase) return failed<std::size_t>(IoError{IoErrorCode::InvalidArgument, "channels 0xFF00 and above are reserved"});
        if (data.size() > core_->options.max_message_bytes) return failed<std::size_t>(IoError{IoErrorCode::LimitExceeded, "message exceeds max_message_bytes"});
        std::vector<std::shared_ptr<PeerLink>> targets;
        {
            auto guard = core_->state.lock();
            for (const auto &[peer, link] : guard->links) {
                if (!except || lowered(*except) != peer) targets.push_back(link);
            }
        }
        const Bytes frame = make_frame(channel, data);
        std::vector<TaskHandle<Result<void>>> sends;
        for (const auto &link : targets) sends.push_back(link->conn.send_all(frame));
        Promise<Result<std::size_t>> promise;
        auto handle = promise.handle();
        if (sends.empty()) {
            (void)promise.set_value(Result<std::size_t>(std::size_t{0}));
            return handle;
        }
        auto joined = when_all(std::move(sends));
        joined.on_done([joined, promise]() mutable {
            try {
                auto results = joined.wait();
                std::size_t accepted = 0;
                for (const auto &result : results) accepted += result.has_value() ? 1 : 0;
                (void)promise.set_value(Result<std::size_t>(accepted));
            } catch (...) {
                (void)promise.set_exception(std::current_exception());
            }
        });
        return handle;
    }

    void PeerNode::disconnect(const PeerId &peer, std::string reason) {
        if (!core_) return;
        std::shared_ptr<PeerLink> link;
        {
            auto guard = core_->state.lock();
            if (auto it = guard->links.find(lowered(peer)); it != guard->links.end()) link = it->second;
        }
        if (link) (void)drop_link(core_, link, std::move(reason));
    }

    void PeerNode::block(const PeerId &peer) {
        if (!core_) return;
        {
            auto guard = core_->state.lock();
            guard->blocked.insert(lowered(peer));
        }
        disconnect(peer, "blocked");
    }

    std::vector<PeerInfo> PeerNode::peers() const {
        std::vector<PeerInfo> out;
        if (!core_) return out;
        auto guard = core_->state.lock();
        for (const auto &[peer, link] : guard->links) out.push_back(link->info);
        return out;
    }

    TaskHandle<std::expected<PeerEvent, IoError>> PeerNode::next_event(CancellationToken cancellation) {
        if (!core_) return failed<PeerEvent>(IoError{IoErrorCode::Closed, "node not started"});
        Promise<Result<PeerEvent>> promise;
        auto handle = promise.handle();
        auto received = core_->events_rx.recv(std::move(cancellation));
        received.on_done([received, promise]() mutable {
            try {
                auto event = received.wait();
                if (event) (void)promise.set_value(Result<PeerEvent>(std::move(*event)));
                else (void)promise.set_value(Result<PeerEvent>(std::unexpected(IoError{IoErrorCode::Closed, "peer node stopped"})));
            } catch (...) {
                (void)promise.set_exception(std::current_exception());
            }
        });
        return handle;
    }

    void PeerNode::stop() noexcept {
        if (!core_ || core_->stopped.exchange(true, std::memory_order_acq_rel)) return;
        try {
            (void)core_->stop_source.cancel();
            core_->listener.close();
            core_->discovery.close();
            std::vector<std::shared_ptr<PeerLink>> links;
            {
                auto guard = core_->state.lock();
                for (auto &[peer, link] : guard->links) links.push_back(link);
                guard->links.clear();
            }
            for (auto &link : links) link->conn.close();
            core_->events_tx.close();
        } catch (...) {
        }
    }

} // namespace SFT::Async
