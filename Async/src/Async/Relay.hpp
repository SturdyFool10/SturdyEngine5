#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Net.hpp>
#include <Async/Size.hpp>
#include <Async/Task.hpp>
#include <Async/Tls.hpp>

namespace SFT::Async {
    using Literals::operator""_MiB;

    // A rendezvous relay for peers that cannot reach each other directly (symmetric NATs, CGNAT, strict firewalls) and
    // where `map_port` / `punch_udp_hole` failed. Both peers dial the relay, name the same *token*, and the relay joins
    // the two connections byte for byte.
    //
    // The relay is not trusted with content: it forwards opaque bytes, and the peers run TLS *through* it with their own
    // keys (`PeerNode::connect_via_relay` does this; with raw `relay_connect` you start TLS yourself). A malicious or
    // compromised relay can drop, delay or cut a session and can see who connected to it and how much was sent -- it
    // cannot read or alter the traffic undetected, nor impersonate a peer whose id you pinned.
    //
    // Tokens are rendezvous names, not credentials: exchange them over your signaling channel, make them unguessable
    // (`random_relay_token`), and never reuse one. Optionally make the relay itself authenticate (`authorize`).

    enum class RelayRole {
        /// The peer that will act as the TLS *client* in the tunnel (it knows whom it expects).
        Dial,
        /// The peer that will act as the TLS *server*.
        Accept,
    };

    /// 32 hex characters of OpenSSL randomness: a fresh, unguessable rendezvous token.
    [[nodiscard]] std::expected<std::string, IoError> random_relay_token();

    struct RelayServerOptions {
        std::string bind_address;      ///< empty: all interfaces
        std::uint16_t port = 0;        ///< 0: OS-chosen
        /// Wraps the hop to the relay in TLS (hides tokens and metadata from the network path). The peers' own
        /// end-to-end TLS inside the tunnel is independent of this.
        std::optional<TlsServerOptions> tls;

        std::size_t max_sessions = 1024;           ///< live joined pairs
        std::size_t max_waiting = 1024;            ///< unpaired connections held at once
        std::size_t max_waiting_per_address = 16;  ///< ... from one client address
        std::chrono::milliseconds pair_timeout{30'000};   ///< how long a peer waits for its partner
        std::chrono::milliseconds idle_timeout{120'000};  ///< a joined session with no bytes either way is cut
        /// A session is cut after relaying this much in total (both directions); 0 = unlimited. Bounds what one token can cost.
        std::uint64_t max_session_bytes = 0;
        std::size_t max_token_bytes = 128;
        /// Called with the token before a peer is accepted; return false to refuse. Use it to tie tokens to your own
        /// accounts or sessions (the relay itself keeps no state between sessions).
        std::function<bool(const std::string &token, const std::string &client_address)> authorize;
    };

    struct RelayMetrics {
        std::uint64_t sessions_started = 0;
        std::uint64_t sessions_active = 0;
        std::uint64_t waiting = 0;
        std::uint64_t bytes_relayed = 0;
        std::uint64_t rejected = 0;   ///< malformed hello, limits, authorize() said no, token busy
        std::uint64_t timed_out = 0;  ///< waited for a partner that never came
    };

    namespace Detail {
        struct RelayState;
    }

    class RelayServer {
      public:
        RelayServer() noexcept = default;
        ~RelayServer() noexcept;
        RelayServer(const RelayServer &) = delete;
        RelayServer &operator=(const RelayServer &) = delete;
        RelayServer(RelayServer &&) noexcept = default;
        RelayServer &operator=(RelayServer &&other) noexcept;

        [[nodiscard]] static TaskHandle<std::expected<RelayServer, IoError>> start(RelayServerOptions options);

        [[nodiscard]] std::expected<std::uint16_t, IoError> local_port() const;
        [[nodiscard]] RelayMetrics metrics() const noexcept;
        /// Stops accepting and cuts every session. Also done by the destructor.
        void stop() noexcept;

      private:
        std::shared_ptr<Detail::RelayState> state_;
    };

    /// Connects to a relay and waits until the partner (the other role, same token) has arrived. Resolves to a plain
    /// byte pipe to that partner: start TLS on it (`start_tls_client` for `Dial`, `start_tls_server` for `Accept`) before
    /// sending anything private. `relay_tls` secures the hop to the relay (when the relay was started with `tls`).
    [[nodiscard]] TaskHandle<std::expected<TcpConnection, IoError>> relay_connect(
        std::string relay_host, std::uint16_t relay_port, std::string token, RelayRole role, std::optional<TlsClientOptions> relay_tls = std::nullopt,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(35'000), CancellationToken cancellation = {});

} // namespace SFT::Async
