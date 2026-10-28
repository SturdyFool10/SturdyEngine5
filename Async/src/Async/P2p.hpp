#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Net.hpp>
#include <Async/Relay.hpp>
#include <Async/Size.hpp>
#include <Async/Task.hpp>
#include <Async/Tls.hpp>

namespace SFT::Async {
    using Literals::operator""_MiB;

    /// A peer's identity: the lowercase hex SHA-256 of its self-signed certificate (64 characters). It is
    /// self-certifying -- TLS proves the other side holds the matching private key -- so a connection to
    /// `PeerId X` is a connection to whoever generated X's key, whatever address it came from.
    using PeerId = std::string;

    struct PeerNodeOptions {
        /// Peers only talk to nodes with the same network id, so unrelated applications on one LAN or DHT-less
        /// swarm never mix. Required.
        std::string network_id;
        std::string display_name;
        /// Long-term identity. Persist `TlsIdentity` between runs to keep the same `PeerId`; when unset an
        /// ephemeral identity is generated.
        std::optional<TlsIdentity> identity;

        bool listen = true;
        std::string bind_address;          ///< empty: all interfaces
        std::uint16_t port = 0;            ///< 0: OS-chosen (see `listen_port()`)

        std::size_t max_peers = 64;
        /// Inbound connections still handshaking at once; more are dropped immediately.
        std::size_t max_pending_inbound = 32;
        /// Largest application message (a frame bigger than this closes the link).
        std::size_t max_message_bytes = 1_MiB;
        /// Peers are pinged this often and dropped after `idle_timeout` without any traffic.
        std::chrono::milliseconds ping_interval{15'000};
        std::chrono::milliseconds idle_timeout{45'000};
        std::chrono::milliseconds handshake_timeout{10'000};
        /// Events buffered for the application; when full, peers' reader loops wait (TCP back-pressure).
        std::size_t event_queue = 1024;

        /// If non-empty only these peers may connect or be connected to.
        std::vector<PeerId> allowed_peers;

        /// Share addresses of connected peers with each other and dial the ones learned this way.
        bool peer_exchange = true;
        /// Announce on the local network (multicast + broadcast) and listen for other nodes of the same network id.
        bool lan_discovery = false;
        std::uint16_t discovery_port = 47831;
        std::string discovery_group = "239.255.83.31";
        std::chrono::milliseconds discovery_interval{5'000};
        /// Dial discovered / exchanged peers automatically while below `max_peers`.
        bool auto_connect = true;
    };

    struct PeerInfo {
        PeerId id;
        SocketAddress address;             ///< where the link goes (remote endpoint)
        std::string name;
        bool inbound = false;
        /// The port the peer listens on, as it announced (0: it does not accept connections).
        std::uint16_t listen_port = 0;
    };

    enum class PeerEventKind {
        Connected,    ///< `peer` is set
        Disconnected, ///< `peer` and `reason` are set
        Message,      ///< `peer`, `channel`, `data`
        Discovered,   ///< a candidate was learned (LAN announcement or peer exchange); `peer.id` + `peer.address`
    };

    struct PeerEvent {
        PeerEventKind kind = PeerEventKind::Message;
        PeerInfo peer;
        std::uint16_t channel = 0;
        std::vector<std::byte> data;
        std::string reason;
    };

    namespace Detail {
        struct PeerCore;
    }

    /// A peer-to-peer node: it listens, dials, authenticates every link with mutual TLS, and gives the application
    /// reliable ordered messages on numbered channels (0..0xFEFF; higher numbers are reserved for control).
    ///
    /// What this is: encrypted authenticated links, identity by key, LAN discovery, peer exchange, limits and
    /// keep-alive. Two nodes behind NATs can link if one of them is reachable (port forward via `map_port`, public
    /// address -- `stun_query` helps find it) or through a relay both can reach (`connect_via_relay`, end-to-end
    /// encrypted). `punch_udp_hole` (NatTraversal.hpp) is the building block for direct UDP paths; the links here are TCP.
    class PeerNode {
      public:
        PeerNode() noexcept = default;
        ~PeerNode() noexcept;
        PeerNode(const PeerNode &) = delete;
        PeerNode &operator=(const PeerNode &) = delete;
        PeerNode(PeerNode &&) noexcept = default;
        PeerNode &operator=(PeerNode &&other) noexcept;

        [[nodiscard]] static TaskHandle<std::expected<PeerNode, IoError>> start(PeerNodeOptions options);

        [[nodiscard]] const PeerId &id() const noexcept;
        [[nodiscard]] const TlsIdentity &identity() const noexcept;
        [[nodiscard]] std::expected<std::uint16_t, IoError> listen_port() const;

        /// Dials `host:port`. With `expected` the link is only accepted if that peer answers (the certificate is
        /// pinned); without it, whoever answers becomes the peer (trust on first use) -- check `PeerInfo::id`.
        /// Resolves to the existing link if the peer is already connected.
        [[nodiscard]] TaskHandle<std::expected<PeerInfo, IoError>> connect(std::string host, std::uint16_t port,
                                                                           std::optional<PeerId> expected = std::nullopt);

        /// Meets a peer through a relay (`RelayServer`) when neither side can be dialed directly. Both peers call this with
        /// the same `token` (exchanged over your signaling channel) and opposite roles: `Dial` takes the TLS client part and
        /// should pass the other's id as `expected`; `Accept` waits to be dialed. The usual mutual TLS then runs *through*
        /// the relay, so the relay cannot read or alter the link, and pinning `expected` means it cannot substitute a
        /// different peer either. `relay_tls` secures the hop to the relay itself.
        [[nodiscard]] TaskHandle<std::expected<PeerInfo, IoError>> connect_via_relay(std::string relay_host, std::uint16_t relay_port, std::string token,
                                                                                     RelayRole role, std::optional<PeerId> expected = std::nullopt,
                                                                                     std::optional<TlsClientOptions> relay_tls = std::nullopt);

        [[nodiscard]] TaskHandle<std::expected<void, IoError>> send(const PeerId &peer, std::uint16_t channel, std::vector<std::byte> data);
        /// Sends to every connected peer (except `except`); resolves to how many links accepted it.
        [[nodiscard]] TaskHandle<std::expected<std::size_t, IoError>> broadcast(std::uint16_t channel, std::vector<std::byte> data,
                                                                                std::optional<PeerId> except = std::nullopt);

        void disconnect(const PeerId &peer, std::string reason = "disconnected by application");
        /// Refuses the peer from now on and drops it if connected.
        void block(const PeerId &peer);
        [[nodiscard]] std::vector<PeerInfo> peers() const;

        /// The next event. Events are queued (bounded); not consuming them eventually stalls the peers' readers.
        [[nodiscard]] TaskHandle<std::expected<PeerEvent, IoError>> next_event(CancellationToken cancellation = {});

        /// Stops listening, drops every link, and ends the event stream. Also done by the destructor.
        void stop() noexcept;

      private:
        std::shared_ptr<Detail::PeerCore> core_;
    };

} // namespace SFT::Async
