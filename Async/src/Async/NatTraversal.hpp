#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Net.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {

    // =================================================================================================
    // Port mapping: ask the router to forward a public port to this machine
    // =================================================================================================
    //
    // Three protocols, tried in this order unless `methods` says otherwise:
    //   * PCP     (RFC 6887)  -- the modern one, UDP to the gateway on port 5351
    //   * NAT-PMP (RFC 6886)  -- PCP's predecessor, same port
    //   * UPnP IGD            -- SSDP discovery + SOAP over HTTP; the most widely deployed, also the most fragile
    //
    // All of them only talk to the *default gateway* (or the router that answers SSDP, whose control URL must live on the
    // very host that answered), never to a host named by a remote party, so a hostile web page or peer cannot aim them.
    // A mapping is a lease: renew by calling `map_port` again before `lifetime` is half over, and `unmap_port` when done.
    // Many networks (CGNAT, hotels, routers with UPnP off) support none of this: failure is normal, fall back to
    // `stun_query` + `punch_udp_hole`, or a relay (`RelayServer`).

    enum class PortProtocol { Tcp, Udp };
    enum class PortMapMethod { Pcp, NatPmp, Upnp };

    [[nodiscard]] const char *to_string(PortMapMethod method) noexcept;

    struct PortMapOptions {
        /// The router to ask. Empty: the default route's gateway (found on Linux; elsewhere you must set it).
        std::string gateway;
        /// Lease requested. Some routers shorten it; the result says what was granted.
        std::chrono::seconds lifetime{3600};
        /// Public port to request; 0 asks for the same number as the internal port. The router may pick another.
        u16 external_port = 0;
        /// Time allowed per method before moving on to the next.
        std::chrono::milliseconds timeout{3000};
        std::vector<PortMapMethod> methods{PortMapMethod::Pcp, PortMapMethod::NatPmp, PortMapMethod::Upnp};
        /// The port PCP / NAT-PMP routers listen on (5351); changeable for tests and odd setups.
        u16 gateway_port = 5351;
        /// Where UPnP discovery is sent (the SSDP multicast group by default); a router's unicast address also works.
        std::string ssdp_host = "239.255.255.250";
        u16 ssdp_port = 1900;
    };

    struct PortMapping {
        PortProtocol protocol = PortProtocol::Tcp;
        u16 internal_port = 0;
        /// The router's public address and the port it granted. `host` is empty if the router would not tell us its address
        /// (the port is still forwarded).
        SocketAddress external;
        std::chrono::seconds lifetime{0}; ///< 0: the router granted a permanent mapping (UPnP only) -- remove it yourself
        PortMapMethod method = PortMapMethod::Pcp;

        // Needed to renew or remove it.
        std::string gateway;
        std::array<std::uint8_t, 12> pcp_nonce{};
        std::string upnp_control_url;
        std::string upnp_service_type;
    };

    /// Asks the router to forward `external` -> this machine's `internal_port`.
    [[nodiscard]] TaskHandle<std::expected<PortMapping, IoError>> map_port(PortProtocol protocol, u16 internal_port, PortMapOptions options = {},
                                                                           CancellationToken cancellation = {});

    /// Removes a mapping made by `map_port`.
    [[nodiscard]] TaskHandle<std::expected<void, IoError>> unmap_port(PortMapping mapping, PortMapOptions options = {},
                                                                      CancellationToken cancellation = {});

    /// The default gateway's address, if the platform lets us read the routing table (Linux).
    [[nodiscard]] std::expected<std::string, IoError> default_gateway();

    // =================================================================================================
    // UDP hole punching
    // =================================================================================================

    struct HolePunchOptions {
        std::chrono::milliseconds timeout{8000};
        /// How often probes are re-sent to every candidate.
        std::chrono::milliseconds interval{150};
    };

    /// Opens a path through both peers' NATs: each side calls this at the same time with the *other* side's candidate
    /// addresses (its `stun_query` result, port mappings, LAN addresses -- exchanged through whatever signaling channel you
    /// have) and a secret both know (exchange it over that same authenticated channel). Probes carry an HMAC of the secret
    /// and a fresh nonce, so only a party that knows the secret can complete the exchange and a recorded probe cannot be
    /// replayed into a later attempt.
    ///
    /// Resolves to the candidate that answered. Afterwards use the *same socket* to talk to it (the NAT mapping belongs to
    /// that socket's port): `socket.send_to(...)` / `receive_from`. Fails with `TimedOut` if no candidate answered --
    /// typical of symmetric NATs on either side, where relaying is the only option.
    ///
    /// `socket` must stay alive and must not be read by anyone else while this runs. Datagrams that are not probes are
    /// discarded meanwhile.
    [[nodiscard]] TaskHandle<std::expected<SocketAddress, IoError>> punch_udp_hole(UdpSocket &socket, std::vector<SocketAddress> candidates,
                                                                                    std::string shared_secret, HolePunchOptions options = {},
                                                                                    CancellationToken cancellation = {});

} // namespace SFT::Async
