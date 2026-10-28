#include <Async/BindingInternal.hpp>

#include <Async/FileHandle.hpp>
#include <Async/FileWatch.hpp>
#include <Async/NatTraversal.hpp>
#include <Async/P2p.hpp>
#include <Async/Process.hpp>
#include <Async/Quic.hpp>
#include <Async/Relay.hpp>
#include <Async/Stream.hpp>

namespace SFT::Async::Binding {

    using namespace Detail;

    namespace {
        constexpr const char *kQuicConn = "quic_conn";
        constexpr const char *kQuicStream = "quic_stream";
        constexpr const char *kQuicListener = "quic_listener";
        constexpr const char *kPeer = "peer";
        constexpr const char *kRelay = "relay_server";
        constexpr const char *kMapping = "mapping";
        constexpr const char *kTcp = "tcp";
        constexpr const char *kUdp = "udp";
        constexpr const char *kFile = "file";
        constexpr const char *kWatcher = "watcher";
        constexpr const char *kProcess = "process";

        std::string endpoint_text(const SocketAddress &a) { return a.host + ":" + std::to_string(a.port); }

        void apply(const QuicLimitsSpec &spec, QuicLimits &limits) {
            limits.idle_timeout = ms(spec.idle_timeout_ms);
            limits.keep_alive = ms(spec.keep_alive_ms);
            limits.handshake_timeout = ms(spec.handshake_timeout_ms);
            limits.max_bidirectional_streams = spec.max_bidirectional_streams;
            limits.max_unidirectional_streams = spec.max_unidirectional_streams;
            limits.stream_receive_window = spec.stream_receive_window;
            limits.connection_receive_window = spec.connection_receive_window;
            limits.datagrams = spec.datagrams;
            limits.stream_send_buffer = static_cast<std::size_t>(spec.stream_send_buffer);
            limits.datagram_receive_queue = static_cast<std::size_t>(spec.datagram_receive_queue);
            limits.datagram_send_queue = static_cast<std::size_t>(spec.datagram_send_queue);
        }
        auto quic_conn_object = [](QuicConnection &&c, Value &out) { out.object = make_object<QuicConnection>(kQuicConn, std::move(c)); };
        auto quic_stream_object = [](QuicStream &&s, Value &out) { out.object = make_object<QuicStream>(kQuicStream, std::move(s)); };
        auto no_value = [](Value &) {};
    } // namespace

    // =================================================================================================================
    // QUIC
    // =================================================================================================================
    OpPtr quic_connect(const std::string &host, std::uint16_t port, const QuicClientSpec &spec) {
        QuicClientOptions o;
        apply(spec.limits, o);
        o.tls = to_options(spec.tls);
        return launch([&](CancellationToken token) { return QuicConnection::connect(host, port, o, std::move(token)); }, quic_conn_object);
    }
    OpPtr quic_listen(const QuicServerSpec &spec) {
        QuicServerOptions o;
        apply(spec.limits, o);
        o.tls = to_options(spec.tls);
        o.bind_address = spec.bind_address;
        o.port = spec.port;
        o.max_connections = static_cast<std::size_t>(spec.max_connections);
        o.max_connections_per_address = static_cast<std::size_t>(spec.max_connections_per_address);
        o.require_address_validation = spec.require_address_validation;
        return launch([&](CancellationToken) { return QuicListener::listen(o); },
                      [](QuicListener &&l, Value &out) { out.object = make_object<QuicListener>(kQuicListener, std::move(l)); });
    }
    OpPtr quic_accept(const ObjectPtr &listener) {
        auto &l = get<QuicListener>(listener, kQuicListener);
        return launch([&, listener](CancellationToken token) { return l.accept(std::move(token)); }, quic_conn_object);
    }
    std::uint16_t quic_listener_port(const ObjectPtr &listener) {
        auto port = get<QuicListener>(listener, kQuicListener).local_port();
        if (!port) throw BindingError(port.error().code, port.error().message);
        return *port;
    }
    Fields quic_listener_metrics(const ObjectPtr &listener) {
        const auto m = get<QuicListener>(listener, kQuicListener).metrics();
        auto n = [](std::uint64_t v) { return std::to_string(v); };
        return {{"connections_accepted", n(m.connections_accepted)}, {"connections_active", n(m.connections_active)}, {"retries_sent", n(m.retries_sent)},
                {"rejected_limit", n(m.rejected_limit)},             {"invalid_tokens", n(m.invalid_tokens)},         {"packets_dropped", n(m.packets_dropped)},
                {"handshakes_failed", n(m.handshakes_failed)}};
    }
    void quic_listener_stop(const ObjectPtr &listener) { get<QuicListener>(listener, kQuicListener).stop(); }

    OpPtr quic_open_stream(const ObjectPtr &connection, bool bidirectional) {
        auto &c = get<QuicConnection>(connection, kQuicConn);
        return launch([&, connection](CancellationToken token) { return c.open_stream(bidirectional, std::move(token)); }, quic_stream_object);
    }
    OpPtr quic_accept_stream(const ObjectPtr &connection) {
        auto &c = get<QuicConnection>(connection, kQuicConn);
        return launch([&, connection](CancellationToken token) { return c.accept_stream(std::move(token)); }, quic_stream_object);
    }
    void quic_send_datagram(const ObjectPtr &connection, Bytes data) {
        auto sent = get<QuicConnection>(connection, kQuicConn).send_datagram(to_native(data));
        if (!sent) throw BindingError(sent.error().code, sent.error().message);
    }
    OpPtr quic_receive_datagram(const ObjectPtr &connection) {
        auto &c = get<QuicConnection>(connection, kQuicConn);
        return launch([&, connection](CancellationToken token) { return c.receive_datagram(std::move(token)); },
                      [](std::vector<std::byte> &&data, Value &out) { out.bytes = from_native(data); });
    }
    std::size_t quic_max_datagram_size(const ObjectPtr &connection) { return get<QuicConnection>(connection, kQuicConn).max_datagram_size(); }
    void quic_close(const ObjectPtr &connection, std::uint64_t code, std::string reason) {
        get<QuicConnection>(connection, kQuicConn).close(code, std::move(reason));
    }
    OpPtr quic_closed(const ObjectPtr &connection) {
        auto &c = get<QuicConnection>(connection, kQuicConn);
        return launch([&, connection](CancellationToken) { return c.closed(); }, [](QuicCloseInfo &&info, Value &out) {
            out.number = static_cast<std::int64_t>(info.code);
            out.text = info.reason;
            out.fields = {{"from_peer", info.from_peer ? "1" : "0"}, {"application", info.application ? "1" : "0"}, {"timed_out", info.timed_out ? "1" : "0"}};
        });
    }
    bool quic_is_open(const ObjectPtr &connection) { return get<QuicConnection>(connection, kQuicConn).is_open(); }
    Fields quic_info(const ObjectPtr &connection) {
        auto &c = get<QuicConnection>(connection, kQuicConn);
        Fields fields;
        if (auto peer = c.peer_address()) fields.emplace_back("peer", endpoint_text(*peer));
        if (auto local = c.local_address()) fields.emplace_back("local", endpoint_text(*local));
        if (auto tls = c.tls_info()) {
            auto more = tls_fields(*tls);
            fields.insert(fields.end(), more.begin(), more.end());
        }
        return fields;
    }
    Fields quic_stats(const ObjectPtr &connection) {
        const auto s = get<QuicConnection>(connection, kQuicConn).stats();
        auto n = [](std::uint64_t v) { return std::to_string(v); };
        return {{"rtt_us", n(static_cast<std::uint64_t>(s.smoothed_rtt.count()))}, {"min_rtt_us", n(static_cast<std::uint64_t>(s.min_rtt.count()))},
                {"bytes_in_flight", n(s.bytes_in_flight)},                         {"cwnd", n(s.congestion_window)},
                {"packets_lost", n(s.packets_lost)},                               {"bytes_sent", n(s.bytes_sent)},
                {"bytes_received", n(s.bytes_received)}};
    }
    OpPtr quic_stream_send(const ObjectPtr &stream, Bytes data) {
        auto &s = get<QuicStream>(stream, kQuicStream);
        return launch([&, stream, data = std::move(data)](CancellationToken token) mutable { return s.send(to_native(data), std::move(token)); }, no_value);
    }
    OpPtr quic_stream_finish(const ObjectPtr &stream) {
        auto &s = get<QuicStream>(stream, kQuicStream);
        return launch([&, stream](CancellationToken) { return s.finish(); }, no_value);
    }
    OpPtr quic_stream_receive(const ObjectPtr &stream, std::size_t max_bytes) {
        auto &s = get<QuicStream>(stream, kQuicStream);
        return launch([&, stream](CancellationToken token) { return s.receive(max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&data, Value &out) { out.bytes = from_native(data); });
    }
    void quic_stream_reset(const ObjectPtr &stream, std::uint64_t code) { get<QuicStream>(stream, kQuicStream).reset(code); }
    void quic_stream_stop_sending(const ObjectPtr &stream, std::uint64_t code) { get<QuicStream>(stream, kQuicStream).stop_sending(code); }
    std::int64_t quic_stream_id(const ObjectPtr &stream) { return get<QuicStream>(stream, kQuicStream).id(); }
    bool quic_stream_is_bidirectional(const ObjectPtr &stream) { return get<QuicStream>(stream, kQuicStream).is_bidirectional(); }

    // =================================================================================================================
    // Peers
    // =================================================================================================================
    namespace {
        Fields peer_info_fields(const PeerInfo &p) {
            return {{"id", p.id}, {"name", p.name}, {"address", endpoint_text(p.address)}, {"inbound", p.inbound ? "1" : "0"}, {"listen_port", std::to_string(p.listen_port)}};
        }
        std::optional<PeerId> optional_id(const std::string &id) { return id.empty() ? std::nullopt : std::optional<PeerId>(id); }
    } // namespace

    OpPtr peer_start(const PeerSpec &spec) {
        PeerNodeOptions o;
        o.network_id = spec.network_id;
        o.display_name = spec.display_name;
        if (spec.has_identity) {
            TlsIdentity identity;
            identity.certificate_pem = spec.identity.certificate_pem;
            identity.private_key_pem = spec.identity.private_key_pem;
            identity.sha256 = spec.identity.sha256;
            o.identity = std::move(identity);
        }
        o.listen = spec.listen;
        o.bind_address = spec.bind_address;
        o.port = spec.port;
        o.max_peers = static_cast<std::size_t>(spec.max_peers);
        o.max_message_bytes = static_cast<std::size_t>(spec.max_message_bytes);
        o.allowed_peers = spec.allowed_peers;
        o.peer_exchange = spec.peer_exchange;
        o.lan_discovery = spec.lan_discovery;
        o.auto_connect = spec.auto_connect;
        o.handshake_timeout = ms(spec.handshake_timeout_ms);
        o.max_pending_inbound = static_cast<std::size_t>(spec.max_pending_inbound);
        o.ping_interval = ms(spec.ping_interval_ms);
        o.idle_timeout = ms(spec.idle_timeout_ms);
        o.event_queue = static_cast<std::size_t>(spec.event_queue);
        o.discovery_port = spec.discovery_port;
        o.discovery_group = spec.discovery_group;
        o.discovery_interval = ms(spec.discovery_interval_ms);
        return launch([&](CancellationToken) { return PeerNode::start(o); }, [](PeerNode &&node, Value &out) {
            out.text = node.id();
            out.object = make_object<PeerNode>(kPeer, std::move(node));
        });
    }
    std::string peer_id(const ObjectPtr &peer) { return get<PeerNode>(peer, kPeer).id(); }
    std::uint16_t peer_listen_port(const ObjectPtr &peer) {
        auto port = get<PeerNode>(peer, kPeer).listen_port();
        if (!port) throw BindingError(port.error().code, port.error().message);
        return *port;
    }
    Identity peer_identity(const ObjectPtr &peer) {
        const auto &i = get<PeerNode>(peer, kPeer).identity();
        return Identity{i.certificate_pem, i.private_key_pem, i.sha256};
    }
    OpPtr peer_connect(const ObjectPtr &peer, const std::string &host, std::uint16_t port, const std::string &expected_id) {
        auto &p = get<PeerNode>(peer, kPeer);
        return launch([&, peer](CancellationToken) { return p.connect(host, port, optional_id(expected_id)); },
                      [](PeerInfo &&info, Value &out) { out.fields = peer_info_fields(info); });
    }
    OpPtr peer_connect_via_relay(const ObjectPtr &peer, const std::string &relay_host, std::uint16_t relay_port, const std::string &token, bool dial,
                                 const std::string &expected_id, const TlsClientSpec *relay_tls) {
        auto &p = get<PeerNode>(peer, kPeer);
        const std::optional<TlsClientOptions> tls = relay_tls ? std::optional<TlsClientOptions>(to_options(*relay_tls)) : std::nullopt;
        return launch([&, peer](CancellationToken) {
            return p.connect_via_relay(relay_host, relay_port, token, dial ? RelayRole::Dial : RelayRole::Accept, optional_id(expected_id), tls);
        }, [](PeerInfo &&info, Value &out) { out.fields = peer_info_fields(info); });
    }
    OpPtr peer_send(const ObjectPtr &peer, const std::string &target_id, std::uint16_t channel, Bytes data) {
        auto &p = get<PeerNode>(peer, kPeer);
        return launch([&, peer](CancellationToken) { return p.send(target_id, channel, to_native(data)); }, no_value);
    }
    OpPtr peer_broadcast(const ObjectPtr &peer, std::uint16_t channel, Bytes data, const std::string &except_id) {
        auto &p = get<PeerNode>(peer, kPeer);
        return launch([&, peer](CancellationToken) { return p.broadcast(channel, to_native(data), optional_id(except_id)); },
                      [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr peer_next_event(const ObjectPtr &peer) {
        auto &p = get<PeerNode>(peer, kPeer);
        return launch([&, peer](CancellationToken token) { return p.next_event(std::move(token)); }, [](PeerEvent &&event, Value &out) {
            out.number = static_cast<std::int64_t>(event.kind);
            out.bytes = from_native(event.data);
            out.fields = {{"peer", event.peer.id},
                          {"channel", std::to_string(event.channel)},
                          {"name", event.peer.name},
                          {"address", endpoint_text(event.peer.address)},
                          {"reason", event.reason},
                          {"inbound", event.peer.inbound ? "1" : "0"}};
        });
    }
    Fields peer_list(const ObjectPtr &peer) {
        Fields fields;
        std::size_t index = 0;
        for (const auto &p : get<PeerNode>(peer, kPeer).peers()) {
            const std::string n = std::to_string(index++);
            fields.emplace_back("id:" + n, p.id);
            fields.emplace_back("name:" + n, p.name);
            fields.emplace_back("address:" + n, endpoint_text(p.address));
            fields.emplace_back("inbound:" + n, p.inbound ? "1" : "0");
            fields.emplace_back("listen_port:" + n, std::to_string(p.listen_port));
        }
        return fields;
    }
    void peer_disconnect(const ObjectPtr &peer, const std::string &id, const std::string &reason) {
        if (reason.empty()) get<PeerNode>(peer, kPeer).disconnect(id);
        else get<PeerNode>(peer, kPeer).disconnect(id, reason);
    }
    void peer_block(const ObjectPtr &peer, const std::string &id) { get<PeerNode>(peer, kPeer).block(id); }
    void peer_stop(const ObjectPtr &peer) { get<PeerNode>(peer, kPeer).stop(); }

    // =================================================================================================================
    // Relay, NAT traversal
    // =================================================================================================================
    ObjectPtr relay_server_start(const RelaySpec &spec, RelayAuthorizeFn authorize) {
        RelayServerOptions o;
        o.bind_address = spec.bind_address;
        o.port = spec.port;
        if (spec.use_tls) o.tls = to_options(spec.tls);
        o.max_sessions = static_cast<std::size_t>(spec.max_sessions);
        o.max_waiting = static_cast<std::size_t>(spec.max_waiting);
        o.max_waiting_per_address = static_cast<std::size_t>(spec.max_waiting_per_address);
        o.pair_timeout = ms(spec.pair_timeout_ms);
        o.idle_timeout = ms(spec.idle_timeout_ms);
        o.max_session_bytes = spec.max_session_bytes;
        o.max_token_bytes = static_cast<std::size_t>(spec.max_token_bytes);
        if (authorize) {
            o.authorize = [authorize = std::move(authorize)](const std::string &token, const std::string &client) {
                try {
                    return authorize(token, client);
                } catch (...) {
                    return false; // a failing check refuses the client
                }
            };
        }
        auto started = RelayServer::start(std::move(o)).wait();
        if (!started) throw BindingError(started.error().code, started.error().message);
        return make_object<RelayServer>(kRelay, std::move(*started));
    }
    std::uint16_t relay_server_port(const ObjectPtr &relay) {
        auto port = get<RelayServer>(relay, kRelay).local_port();
        if (!port) throw BindingError(port.error().code, port.error().message);
        return *port;
    }
    Fields relay_server_metrics(const ObjectPtr &relay) {
        const auto m = get<RelayServer>(relay, kRelay).metrics();
        auto n = [](std::uint64_t v) { return std::to_string(v); };
        return {{"sessions_started", n(m.sessions_started)}, {"sessions_active", n(m.sessions_active)}, {"waiting", n(m.waiting)},
                {"bytes_relayed", n(m.bytes_relayed)},       {"rejected", n(m.rejected)},               {"timed_out", n(m.timed_out)}};
    }
    void relay_server_stop(const ObjectPtr &relay) { get<RelayServer>(relay, kRelay).stop(); }
    std::string random_relay_token() {
        auto token = SFT::Async::random_relay_token();
        if (!token) throw BindingError(token.error().code, token.error().message);
        return *token;
    }
    OpPtr relay_connect(const std::string &host, std::uint16_t port, const std::string &token, bool dial, const TlsClientSpec *relay_tls, std::int64_t timeout_ms) {
        const std::optional<TlsClientOptions> tls = relay_tls ? std::optional<TlsClientOptions>(to_options(*relay_tls)) : std::nullopt;
        return launch([&](CancellationToken cancel) {
            return SFT::Async::relay_connect(host, port, token, dial ? RelayRole::Dial : RelayRole::Accept, tls, ms(timeout_ms), std::move(cancel));
        }, [](TcpConnection &&c, Value &out) { out.object = make_object<TcpConnection>(kTcp, std::move(c)); });
    }

    namespace {
        PortMapOptions to_options(const PortMapSpec &spec) {
            PortMapOptions o;
            o.gateway = spec.gateway;
            o.lifetime = std::chrono::seconds(spec.lifetime_s);
            o.external_port = spec.external_port;
            o.timeout = ms(spec.timeout_ms);
            o.methods.clear();
            if (spec.methods & 1u) o.methods.push_back(PortMapMethod::Pcp);
            if (spec.methods & 2u) o.methods.push_back(PortMapMethod::NatPmp);
            if (spec.methods & 4u) o.methods.push_back(PortMapMethod::Upnp);
            o.gateway_port = spec.gateway_port;
            o.ssdp_host = spec.ssdp_host;
            o.ssdp_port = spec.ssdp_port;
            return o;
        }
    } // namespace

    OpPtr map_port(bool tcp, std::uint16_t internal_port, const PortMapSpec &spec) {
        const PortMapOptions options = to_options(spec);
        return launch([&](CancellationToken token) { return SFT::Async::map_port(tcp ? PortProtocol::Tcp : PortProtocol::Udp, internal_port, options, std::move(token)); },
                      [](PortMapping &&m, Value &out) {
                          out.number = m.lifetime.count();
                          out.fields = {{"external_host", m.external.host}, {"external_port", std::to_string(m.external.port)}, {"method", to_string(m.method)},
                                        {"gateway", m.gateway}};
                          out.object = make_object<PortMapping>(kMapping, std::move(m));
                      });
    }
    OpPtr unmap_port(const ObjectPtr &mapping, const PortMapSpec &spec) {
        PortMapping copy = get<PortMapping>(mapping, kMapping);
        const PortMapOptions options = to_options(spec);
        return launch([&](CancellationToken token) { return SFT::Async::unmap_port(std::move(copy), options, std::move(token)); }, no_value);
    }
    std::string default_gateway() {
        auto gateway = SFT::Async::default_gateway();
        return gateway ? *gateway : std::string{};
    }
    OpPtr punch_udp_hole(const ObjectPtr &socket, const std::vector<std::pair<std::string, std::uint16_t>> &candidates, const std::string &secret,
                         std::int64_t timeout_ms) {
        auto &s = get<UdpSocket>(socket, kUdp);
        std::vector<SocketAddress> list;
        for (const auto &[host, port] : candidates) list.push_back({host, port});
        HolePunchOptions options;
        options.timeout = ms(timeout_ms);
        // The socket object is captured by the completion lambda so it outlives the attempt.
        return launch([&](CancellationToken token) { return SFT::Async::punch_udp_hole(s, std::move(list), secret, options, std::move(token)); },
                      [socket](SocketAddress &&address, Value &out) {
                          out.text = address.host;
                          out.number = address.port;
                      });
    }

    // =================================================================================================================
    // Files, watching
    // =================================================================================================================
    bool file_is_open(const ObjectPtr &file) { return get<File>(file, kFile).is_open(); }
    OpPtr file_open(const std::string &path, std::uint32_t mode) {
        OpenMode m = static_cast<OpenMode>(mode);
        return launch([&](CancellationToken) { return File::open(path, m); }, [](File &&f, Value &out) { out.object = make_object<File>(kFile, std::move(f)); });
    }
    OpPtr file_read(const ObjectPtr &file, std::size_t max_bytes) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken token) { return f.read(max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&d, Value &out) { out.bytes = from_native(d); });
    }
    OpPtr file_read_at(const ObjectPtr &file, std::uint64_t offset, std::size_t max_bytes) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken token) { return f.read_at(offset, max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&d, Value &out) { out.bytes = from_native(d); });
    }
    OpPtr file_read_to_end(const ObjectPtr &file, std::size_t max_bytes) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken token) { return f.read_to_end(max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&d, Value &out) { out.bytes = from_native(d); });
    }
    OpPtr file_write(const ObjectPtr &file, Bytes data) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken token) { return f.write(to_native(data), std::move(token)); },
                      [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr file_write_at(const ObjectPtr &file, std::uint64_t offset, Bytes data) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken token) { return f.write_at(offset, to_native(data), std::move(token)); },
                      [](std::size_t n, Value &out) { out.number = static_cast<std::int64_t>(n); });
    }
    OpPtr file_seek(const ObjectPtr &file, std::int64_t offset, int origin) {
        auto &f = get<File>(file, kFile);
        const SeekOrigin o = origin == 1 ? SeekOrigin::Current : origin == 2 ? SeekOrigin::End : SeekOrigin::Begin;
        return launch([&, file](CancellationToken) { return f.seek(offset, o); }, [](std::uint64_t position, Value &out) { out.number = static_cast<std::int64_t>(position); });
    }
    OpPtr file_size(const ObjectPtr &file) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken) { return f.size(); }, [](std::uint64_t size, Value &out) { out.number = static_cast<std::int64_t>(size); });
    }
    OpPtr file_set_len(const ObjectPtr &file, std::uint64_t length) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken) { return f.set_len(length); }, no_value);
    }
    OpPtr file_sync(const ObjectPtr &file, bool data_only) {
        auto &f = get<File>(file, kFile);
        return launch([&, file](CancellationToken) { return f.sync(data_only); }, no_value);
    }
    void file_close(const ObjectPtr &file) { get<File>(file, kFile).close(); }

    ObjectPtr watch_path(const std::string &path, const WatchSpec &spec) {
        FileWatchOptions o;
        o.recursive = spec.recursive;
        o.debounce = ms(spec.debounce_ms);
        o.force_polling = spec.force_polling;
        o.poll_interval = ms(spec.poll_interval_ms);
        o.max_pending_events = static_cast<std::size_t>(spec.max_pending_events);
        auto watcher = FileWatcher::watch(path, o);
        if (!watcher) throw BindingError(watcher.error().code, watcher.error().message);
        return make_object<FileWatcher>(kWatcher, std::move(*watcher));
    }
    OpPtr watch_next(const ObjectPtr &watcher) {
        auto &w = get<FileWatcher>(watcher, kWatcher);
        return launch([&, watcher](CancellationToken token) { return w.next(std::move(token)); }, [](FileEvent &&e, Value &out) {
            out.number = static_cast<std::int64_t>(e.kind);
            out.text = e.path;
            out.fields = {{"old_path", e.old_path}, {"is_directory", e.is_directory ? "1" : "0"}};
        });
    }
    bool watch_uses_native_backend(const ObjectPtr &watcher) { return get<FileWatcher>(watcher, kWatcher).uses_native_backend(); }
    ObjectPtr watch_events_stream(const ObjectPtr &watcher) {
        auto stream = get<FileWatcher>(watcher, kWatcher).events().map([](FileEvent e) {
            auto frame = [](const std::string &text) { return Bytes(text.begin(), text.end()); };
            return pack_frames({frame(std::to_string(static_cast<int>(e.kind))), frame(e.path), frame(e.old_path), frame(e.is_directory ? "1" : "0")});
        });
        return make_object<Stream<Bytes>>("stream", std::move(stream));
    }
    void watch_close(const ObjectPtr &watcher) { get<FileWatcher>(watcher, kWatcher).close(); }

    // =================================================================================================================
    // Processes
    // =================================================================================================================
    namespace {
        ProcessOptions to_options(const ProcessSpec &spec) {
            ProcessOptions o;
            o.program = spec.program;
            o.args = spec.args;
            o.working_directory = spec.working_directory;
            for (const auto &[k, v] : spec.environment) o.environment[k] = v;
            o.clear_environment = spec.clear_environment;
            o.kill_on_drop = spec.kill_on_drop;
            auto mode = [](StdioKind kind) { return kind == StdioKind::Null ? StdioMode::Null : kind == StdioKind::Inherit ? StdioMode::Inherit : StdioMode::Pipe; };
            o.stdin_mode = mode(spec.stdin_mode);
            o.stdout_mode = mode(spec.stdout_mode);
            o.stderr_mode = mode(spec.stderr_mode);
            return o;
        }
        Fields exit_fields(const ExitStatus &s) {
            return {{"signaled", s.signaled ? "1" : "0"}, {"success", s.success() ? "1" : "0"}, {"signal", std::to_string(s.signal)}};
        }
    } // namespace

    OpPtr process_run(const ProcessSpec &spec, Bytes stdin_data, std::int64_t timeout_ms, std::uint64_t max_output_bytes) {
        RunLimits limits;
        limits.timeout = ms(timeout_ms);
        if (max_output_bytes != 0) limits.max_output_bytes = static_cast<std::size_t>(max_output_bytes);
        const ProcessOptions options = to_options(spec);
        return launch([&](CancellationToken token) { return run_process(options, to_native(stdin_data), limits, std::move(token)); }, [](ProcessOutput &&r, Value &out) {
            out.bytes = from_native(r.stdout_data);
            out.text = r.stderr_text();
            out.number = r.status.code;
            out.fields = exit_fields(r.status);
        });
    }
    ObjectPtr process_spawn(const ProcessSpec &spec) {
        const ProcessOptions options = to_options(spec);
        auto process = Process::spawn(options);
        if (!process) throw BindingError(process.error().code, process.error().message);
        return make_object<Process>(kProcess, std::move(*process));
    }
    OpPtr process_write_stdin(const ObjectPtr &process, Bytes data) {
        auto &p = get<Process>(process, kProcess);
        return launch([&, process](CancellationToken token) { return p.write_stdin(to_native(data), std::move(token)); }, no_value);
    }
    void process_close_stdin(const ObjectPtr &process) { get<Process>(process, kProcess).close_stdin(); }
    OpPtr process_read_stdout(const ObjectPtr &process, std::size_t max_bytes) {
        auto &p = get<Process>(process, kProcess);
        return launch([&, process](CancellationToken token) { return p.read_stdout(max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&d, Value &out) { out.bytes = from_native(d); });
    }
    OpPtr process_read_stderr(const ObjectPtr &process, std::size_t max_bytes) {
        auto &p = get<Process>(process, kProcess);
        return launch([&, process](CancellationToken token) { return p.read_stderr(max_bytes, std::move(token)); },
                      [](std::vector<std::byte> &&d, Value &out) { out.bytes = from_native(d); });
    }
    OpPtr process_wait(const ObjectPtr &process) {
        auto &p = get<Process>(process, kProcess);
        return launch([&, process](CancellationToken token) { return p.wait(std::move(token)); }, [](ExitStatus &&s, Value &out) {
            out.number = s.code;
            out.fields = exit_fields(s);
        });
    }
    std::int64_t process_id(const ObjectPtr &process) { return get<Process>(process, kProcess).id(); }
    void process_request_exit(const ObjectPtr &process) { get<Process>(process, kProcess).request_exit(); }
    void process_kill(const ObjectPtr &process) { get<Process>(process, kProcess).kill(); }

} // namespace SFT::Async::Binding
