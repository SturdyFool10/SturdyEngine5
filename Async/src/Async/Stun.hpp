#pragma once

#include <chrono>
#include <expected>
#include <string>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Net.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {

    /// Asks a STUN server (RFC 5389 Binding request) which public address and port it sees this machine as.
    /// Useful for advertising a reachable address to peers behind NAT. Retransmits with back-off until `timeout`.
    ///
    /// The result is the NAT's mapping for the probe socket only: whether other hosts can reach you through it
    /// depends on the NAT type, and a firewall or symmetric NAT may still block inbound connections. Reliable
    /// traversal needs ICE/relay support, which this does not provide.
    ///
    /// If `socket` is given it is used (so the mapping describes that socket's port); otherwise a temporary
    /// ephemeral socket is.
    [[nodiscard]] TaskHandle<std::expected<SocketAddress, IoError>> stun_query(
        std::string server_host = "stun.l.google.com", u16 server_port = 19302,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(3000), CancellationToken cancellation = {});

} // namespace SFT::Async
