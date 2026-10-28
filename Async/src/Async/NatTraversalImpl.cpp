#include <Async/HttpClient.hpp>
#include <Async/NatTraversal.hpp>
#include <Async/Promise.hpp>
#include <Async/Timer.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <Async/NetInternal.hpp>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

namespace SFT::Async {

    const char *to_string(PortMapMethod method) noexcept {
        switch (method) {
            case PortMapMethod::Pcp: return "PCP";
            case PortMapMethod::NatPmp: return "NAT-PMP";
            case PortMapMethod::Upnp: return "UPnP";
        }
        return "?";
    }

    namespace {

        using Bytes = std::vector<std::byte>;

        // ---- wire helpers ---------------------------------------------------------------------------------------
        void put8(Bytes &out, unsigned v) { out.push_back(static_cast<std::byte>(v)); }
        void put16(Bytes &out, unsigned v) {
            put8(out, v >> 8);
            put8(out, v & 0xFF);
        }
        void put32(Bytes &out, std::uint32_t v) {
            put16(out, v >> 16);
            put16(out, v & 0xFFFF);
        }
        unsigned get8(const Bytes &in, std::size_t at) { return std::to_integer<unsigned>(in[at]); }
        unsigned get16(const Bytes &in, std::size_t at) { return (get8(in, at) << 8) | get8(in, at + 1); }
        std::uint32_t get32(const Bytes &in, std::size_t at) { return (static_cast<std::uint32_t>(get16(in, at)) << 16) | get16(in, at + 2); }

        IoError failure(IoErrorCode code, std::string message) { return IoError{code, std::move(message)}; }

        bool parse_ipv4(const std::string &text, std::array<std::uint8_t, 4> &out) {
            std::size_t position = 0;
            for (int i = 0; i < 4; ++i) {
                unsigned value = 0;
                const char *begin = text.data() + position;
                const auto [end, ec] = std::from_chars(begin, text.data() + text.size(), value);
                if (ec != std::errc{} || end == begin || value > 255) return false;
                out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value);
                position = static_cast<std::size_t>(end - text.data());
                if (i < 3) {
                    if (position >= text.size() || text[position] != '.') return false;
                    ++position;
                }
            }
            return position == text.size();
        }

        std::string format_ipv4(const std::array<std::uint8_t, 4> &o) {
            return std::to_string(o[0]) + "." + std::to_string(o[1]) + "." + std::to_string(o[2]) + "." + std::to_string(o[3]);
        }

        /// The source address the OS would use to reach `host:port` (a connected UDP socket sends nothing; it only consults
        /// the routing table). `UdpSocket::local_address` is no help here: it reports the bind address, e.g. 0.0.0.0.
        std::expected<SocketAddress, IoError> local_address_towards(const std::string &host, u16 port) {
            Detail::ErrorCode ec;
            const auto address = Detail::asio::ip::make_address(host, ec);
            if (ec) return std::unexpected(failure(IoErrorCode::InvalidArgument, "not a numeric address: " + host));
            Detail::asio::io_context context;
            Detail::udp::socket probe(context);
            probe.open(address.is_v4() ? Detail::udp::v4() : Detail::udp::v6(), ec);
            if (!ec) probe.connect(Detail::udp::endpoint(address, port), ec);
            if (ec) return std::unexpected(failure(IoErrorCode::Unsupported, "no route to " + host + ": " + ec.message()));
            const auto local = probe.local_endpoint(ec);
            if (ec) return std::unexpected(failure(IoErrorCode::Unknown, "cannot read the local address: " + ec.message()));
            return SocketAddress{local.address().to_string(), local.port()};
        }

        // ---- the gateway ----------------------------------------------------------------------------------------
        TaskHandle<std::expected<std::string, IoError>> gateway_address(const std::string &configured) {
            std::string name = configured;
            if (name.empty()) {
                auto found = default_gateway();
                if (!found) co_return std::unexpected(found.error());
                name = std::move(*found);
            }
            auto resolved = co_await resolve_host(name, 0);
            if (!resolved) co_return std::unexpected(resolved.error());
            if (resolved->empty()) co_return std::unexpected(failure(IoErrorCode::NotFound, "gateway does not resolve: " + name));
            co_return std::expected<std::string, IoError>((*resolved)[0].host);
        }

        /// A receive that gives up after `wait` instead of blocking indefinitely. `nullopt` = nothing arrived in time.
        TaskHandle<std::expected<std::optional<UdpDatagram>, IoError>> receive_within(UdpSocket &socket, std::size_t max_bytes,
                                                                                     std::chrono::milliseconds wait, CancellationToken outer) {
            CancellationSource stop;
            auto timer_stop = std::make_shared<CancellationSource>();
            auto timer = sleep_for(wait, timer_stop->token());
            timer.on_done([timer, stop, timer_stop]() mutable {
                try {
                    timer.wait();
                    (void)stop.cancel();
                } catch (...) {
                }
            });
            std::optional<CancellationRegistration> outer_registration;
            if (outer.can_be_cancelled()) outer_registration = outer.on_cancel([stop]() mutable { (void)stop.cancel(); });
            bool timed_out = false;
            std::expected<UdpDatagram, IoError> datagram = std::unexpected(failure(IoErrorCode::Unknown, "unreachable"));
            try {
                datagram = co_await socket.receive_from(max_bytes, stop.token());
            } catch (const IoOperationCancelled &) {
                timed_out = true;
            }
            (void)timer_stop->cancel();
            outer_registration.reset();
            if (timed_out) {
                // (an exception thrown here would be the caller's cancellation, not ours)
                if (outer.is_cancelled()) throw IoOperationCancelled{};
                co_return std::expected<std::optional<UdpDatagram>, IoError>(std::nullopt);
            }
            if (!datagram) co_return std::unexpected(datagram.error());
            co_return std::expected<std::optional<UdpDatagram>, IoError>(std::move(*datagram));
        }

        /// Sends `request` to the gateway, retransmitting with back-off (250 ms, 500 ms, ...), until `parse` accepts a
        /// datagram from the gateway itself. Datagrams from anyone else are ignored.
        template <typename T, typename Parse>
        TaskHandle<std::expected<T, IoError>> gateway_exchange(std::shared_ptr<UdpSocket> socket, std::string gateway, u16 port, Bytes request,
                                                              std::chrono::milliseconds timeout, Parse parse, CancellationToken cancellation) {
            const auto deadline = Clock::now() + timeout;
            std::chrono::milliseconds backoff{250};
            while (Clock::now() < deadline) {
                auto sent = co_await socket->send_to(gateway, port, request);
                if (!sent) co_return std::unexpected(sent.error());
                const auto round_end = (std::min)(Clock::now() + backoff, deadline);
                while (Clock::now() < round_end) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(round_end - Clock::now());
                    auto received = co_await receive_within(*socket, 1100, (std::max)(left, std::chrono::milliseconds(1)), cancellation);
                    if (!received) co_return std::unexpected(received.error());
                    if (!*received) break;
                    UdpDatagram &datagram = **received;
                    if (datagram.sender_host != gateway) continue;
                    std::expected<std::optional<T>, IoError> parsed = parse(datagram.data);
                    if (!parsed) co_return std::unexpected(parsed.error());
                    if (*parsed) co_return std::expected<T, IoError>(std::move(**parsed));
                }
                backoff *= 2;
            }
            co_return std::unexpected(failure(IoErrorCode::TimedOut, "the gateway did not answer"));
        }

        TaskHandle<std::expected<std::shared_ptr<UdpSocket>, IoError>> socket_to_gateway(const std::string &gateway) {
            auto bound = co_await UdpSocket::bind("0.0.0.0", 0);
            if (!bound) co_return std::unexpected(bound.error());
            auto socket = std::make_shared<UdpSocket>(std::move(*bound));
            co_return std::expected<std::shared_ptr<UdpSocket>, IoError>(std::move(socket));
        }

        // ---- NAT-PMP (RFC 6886) -----------------------------------------------------------------------------------
        IoError natpmp_error(unsigned result) {
            switch (result) {
                case 1: return failure(IoErrorCode::Unsupported, "NAT-PMP: unsupported version");
                case 2: return failure(IoErrorCode::PermissionDenied, "NAT-PMP: refused by the router (not authorized)");
                case 3: return failure(IoErrorCode::ConnectionReset, "NAT-PMP: the router has no external network");
                case 4: return failure(IoErrorCode::LimitExceeded, "NAT-PMP: the router is out of mappings");
                default: return failure(IoErrorCode::ProtocolError, "NAT-PMP: result code " + std::to_string(result));
            }
        }

        TaskHandle<std::expected<PortMapping, IoError>> map_natpmp(PortProtocol protocol, u16 internal_port, PortMapOptions options, std::string gateway,
                                                                   CancellationToken cancellation, std::uint32_t lifetime_seconds) {
            auto made = co_await socket_to_gateway(gateway);
            if (!made) co_return std::unexpected(made.error());
            auto socket = *made;

            // Public address first (op 0), then the mapping (op 1 = UDP, 2 = TCP).
            std::string external_host;
            {
                Bytes request;
                put8(request, 0);
                put8(request, 0);
                auto answered = co_await gateway_exchange<std::string>(
                    socket, gateway, options.gateway_port, std::move(request), options.timeout,
                    [](const Bytes &packet) -> std::expected<std::optional<std::string>, IoError> {
                        if (packet.size() < 12 || get8(packet, 0) != 0 || get8(packet, 1) != 128) return std::optional<std::string>{};
                        if (const unsigned result = get16(packet, 2); result != 0) return std::unexpected(natpmp_error(result));
                        return std::optional<std::string>(format_ipv4({static_cast<std::uint8_t>(get8(packet, 8)), static_cast<std::uint8_t>(get8(packet, 9)),
                                                                       static_cast<std::uint8_t>(get8(packet, 10)), static_cast<std::uint8_t>(get8(packet, 11))}));
                    },
                    cancellation);
                if (!answered) co_return std::unexpected(answered.error());
                external_host = std::move(*answered);
            }

            Bytes request;
            put8(request, 0);
            put8(request, protocol == PortProtocol::Udp ? 1 : 2);
            put16(request, 0);
            put16(request, internal_port);
            put16(request, options.external_port != 0 ? options.external_port : internal_port);
            put32(request, lifetime_seconds);
            struct Granted {
                u16 external_port;
                std::uint32_t lifetime;
            };
            const unsigned expected_op = 128u + (protocol == PortProtocol::Udp ? 1u : 2u);
            auto answered = co_await gateway_exchange<Granted>(
                socket, gateway, options.gateway_port, std::move(request), options.timeout,
                [internal_port, expected_op](const Bytes &packet) -> std::expected<std::optional<Granted>, IoError> {
                    if (packet.size() < 16 || get8(packet, 0) != 0 || get8(packet, 1) != expected_op || get16(packet, 8) != internal_port) {
                        return std::optional<Granted>{};
                    }
                    if (const unsigned result = get16(packet, 2); result != 0) return std::unexpected(natpmp_error(result));
                    return std::optional<Granted>(Granted{static_cast<u16>(get16(packet, 10)), get32(packet, 12)});
                },
                cancellation);
            if (!answered) co_return std::unexpected(answered.error());

            PortMapping mapping;
            mapping.protocol = protocol;
            mapping.internal_port = internal_port;
            mapping.external = {external_host, answered->external_port};
            mapping.lifetime = std::chrono::seconds(answered->lifetime);
            mapping.method = PortMapMethod::NatPmp;
            mapping.gateway = gateway;
            co_return std::expected<PortMapping, IoError>(std::move(mapping));
        }

        // ---- PCP (RFC 6887) ---------------------------------------------------------------------------------------
        IoError pcp_error(unsigned result) {
            switch (result) {
                case 1: return failure(IoErrorCode::Unsupported, "PCP: unsupported version");
                case 2: return failure(IoErrorCode::PermissionDenied, "PCP: refused by the router (not authorized)");
                case 3: return failure(IoErrorCode::InvalidArgument, "PCP: malformed request");
                case 4: return failure(IoErrorCode::Unsupported, "PCP: unsupported opcode");
                case 8: return failure(IoErrorCode::ConnectionReset, "PCP: the router has no external network");
                case 9: return failure(IoErrorCode::LimitExceeded, "PCP: too many mappings");
                case 11: return failure(IoErrorCode::Unsupported, "PCP: unsupported protocol");
                case 12: return failure(IoErrorCode::LimitExceeded, "PCP: mapping quota exceeded");
                default: return failure(IoErrorCode::ProtocolError, "PCP: result code " + std::to_string(result));
            }
        }

        void put_ipv4_mapped(Bytes &out, const std::array<std::uint8_t, 4> &address) {
            for (int i = 0; i < 10; ++i) put8(out, 0);
            put16(out, 0xFFFF);
            for (std::uint8_t octet : address) put8(out, octet);
        }

        TaskHandle<std::expected<PortMapping, IoError>> map_pcp(PortProtocol protocol, u16 internal_port, PortMapOptions options, std::string gateway,
                                                                std::array<std::uint8_t, 12> nonce, CancellationToken cancellation,
                                                                std::uint32_t lifetime_seconds) {
            auto made = co_await socket_to_gateway(gateway);
            if (!made) co_return std::unexpected(made.error());
            auto socket = *made;
            auto local = local_address_towards(gateway, options.gateway_port);
            std::array<std::uint8_t, 4> client{};
            if (!local || !parse_ipv4(local->host, client)) co_return std::unexpected(failure(IoErrorCode::Unsupported, "PCP: IPv4 gateways only"));

            Bytes request;
            put8(request, 2); // version
            put8(request, 1); // R=0, opcode MAP
            put16(request, 0);
            put32(request, lifetime_seconds);
            put_ipv4_mapped(request, client);
            for (std::uint8_t byte : nonce) put8(request, byte);
            put8(request, protocol == PortProtocol::Udp ? 17 : 6);
            put8(request, 0);
            put16(request, 0);
            put16(request, internal_port);
            put16(request, options.external_port != 0 ? options.external_port : internal_port);
            for (int i = 0; i < 16; ++i) put8(request, 0); // no suggested external address

            struct Granted {
                u16 external_port;
                std::string external_host;
                std::uint32_t lifetime;
            };
            auto answered = co_await gateway_exchange<Granted>(
                socket, gateway, options.gateway_port, std::move(request), options.timeout,
                [nonce, internal_port](const Bytes &packet) -> std::expected<std::optional<Granted>, IoError> {
                    // A NAT-PMP-only router answers a PCP request with its own, version 0 "unsupported" reply (RFC 6887 section 9).
                    if (packet.size() >= 4 && get8(packet, 0) == 0 && get16(packet, 2) == 1) {
                        return std::unexpected(failure(IoErrorCode::Unsupported, "PCP: the router only speaks NAT-PMP"));
                    }
                    if (packet.size() < 60 || (get8(packet, 1) & 0x7F) != 1 || (get8(packet, 1) & 0x80) == 0) return std::optional<Granted>{};
                    if (get8(packet, 0) != 2) {
                        return std::unexpected(failure(IoErrorCode::Unsupported, "PCP: the router answered with another protocol version"));
                    }
                    // The nonce binds the answer to our request: anything else is not for us.
                    if (std::memcmp(packet.data() + 24, nonce.data(), nonce.size()) != 0 || get16(packet, 40) != internal_port) return std::optional<Granted>{};
                    if (const unsigned result = get8(packet, 3); result != 0) return std::unexpected(pcp_error(result));
                    std::string host;
                    const bool mapped = get32(packet, 44) == 0 && get32(packet, 48) == 0 && get16(packet, 52) == 0 && get16(packet, 54) == 0xFFFF;
                    if (mapped) {
                        host = format_ipv4({static_cast<std::uint8_t>(get8(packet, 56)), static_cast<std::uint8_t>(get8(packet, 57)),
                                            static_cast<std::uint8_t>(get8(packet, 58)), static_cast<std::uint8_t>(get8(packet, 59))});
                    }
                    return std::optional<Granted>(Granted{static_cast<u16>(get16(packet, 42)), std::move(host), get32(packet, 4)});
                },
                cancellation);
            if (!answered) co_return std::unexpected(answered.error());

            PortMapping mapping;
            mapping.protocol = protocol;
            mapping.internal_port = internal_port;
            mapping.external = {answered->external_host, answered->external_port};
            mapping.lifetime = std::chrono::seconds(answered->lifetime);
            mapping.method = PortMapMethod::Pcp;
            mapping.gateway = gateway;
            mapping.pcp_nonce = nonce;
            co_return std::expected<PortMapping, IoError>(std::move(mapping));
        }

        // ---- UPnP IGD ---------------------------------------------------------------------------------------------
        std::optional<std::string_view> xml_text(std::string_view xml, std::string_view tag, std::size_t &from) {
            const std::string open = "<" + std::string(tag) + ">";
            const std::string close = "</" + std::string(tag) + ">";
            const auto start = xml.find(open, from);
            if (start == std::string_view::npos) return std::nullopt;
            const auto content = start + open.size();
            const auto stop = xml.find(close, content);
            if (stop == std::string_view::npos) return std::nullopt;
            from = stop + close.size();
            return xml.substr(content, stop - content);
        }

        std::string trim(std::string_view text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
            return std::string(text);
        }

        struct HttpLocation {
            std::string host;
            u16 port = 80;
            std::string path = "/";
        };

        /// Only plain `http://numeric-or-name[:port][/path]` -- IGD devices speak nothing else, and refusing other schemes keeps
        /// a malicious LOCATION from steering us at file:// or https:// endpoints.
        std::optional<HttpLocation> parse_http_location(const std::string &url) {
            constexpr std::string_view scheme = "http://";
            if (url.compare(0, scheme.size(), scheme) != 0) return std::nullopt;
            HttpLocation out;
            const std::size_t authority_end = url.find('/', scheme.size());
            std::string authority = url.substr(scheme.size(), authority_end == std::string::npos ? std::string::npos : authority_end - scheme.size());
            out.path = authority_end == std::string::npos ? "/" : url.substr(authority_end);
            if (authority.empty() || authority.find('@') != std::string::npos || authority.front() == '[') return std::nullopt;
            if (const auto colon = authority.rfind(':'); colon != std::string::npos) {
                unsigned port = 0;
                const auto [end, ec] = std::from_chars(authority.data() + colon + 1, authority.data() + authority.size(), port);
                if (ec != std::errc{} || end != authority.data() + authority.size() || port == 0 || port > 65535) return std::nullopt;
                out.port = static_cast<u16>(port);
                authority.resize(colon);
            }
            out.host = std::move(authority);
            return out;
        }

        std::string http_url(const HttpLocation &where, const std::string &path) { return "http://" + where.host + ":" + std::to_string(where.port) + path; }

        std::string xml_escape(std::string_view text) {
            std::string out;
            for (char c : text) {
                switch (c) {
                    case '&': out += "&amp;"; break;
                    case '<': out += "&lt;"; break;
                    case '>': out += "&gt;"; break;
                    default: out += c;
                }
            }
            return out;
        }

        struct UpnpDevice {
            HttpLocation location;
            std::string control_path;
            std::string service_type;
        };

        std::shared_ptr<HttpClient> make_igd_client(std::chrono::milliseconds timeout) {
            HttpClientOptions options;
            options.timeout = timeout;
            options.max_redirects = 0; // a redirect could point anywhere
            options.keep_alive = false;
            options.max_response_body_bytes = 256 * 1024;
            options.max_response_header_bytes = 16 * 1024;
            options.decompress = false;
            options.http2 = false;
            return std::make_shared<HttpClient>(options);
        }

        TaskHandle<std::expected<UpnpDevice, IoError>> discover_igd(PortMapOptions options, std::string gateway, CancellationToken cancellation) {
            auto bound = co_await UdpSocket::bind("0.0.0.0", 0);
            if (!bound) co_return std::unexpected(bound.error());
            UdpSocket socket = std::move(*bound);
            (void)(co_await socket.set_multicast_ttl(2));

            const std::string target = "urn:schemas-upnp-org:device:InternetGatewayDevice:1";
            std::string search = "M-SEARCH * HTTP/1.1\r\nHOST: " + options.ssdp_host + ":" + std::to_string(options.ssdp_port) +
                                 "\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: " + target + "\r\n\r\n";
            Bytes packet(search.size());
            std::memcpy(packet.data(), search.data(), search.size());

            const auto deadline = Clock::now() + options.timeout;
            auto http = make_igd_client(options.timeout);
            for (int round = 0; round < 3 && Clock::now() < deadline; ++round) {
                auto sent = co_await socket.send_to(options.ssdp_host, options.ssdp_port, packet);
                if (!sent) co_return std::unexpected(sent.error());
                const auto round_end = (std::min)(Clock::now() + options.timeout / 3, deadline);
                while (Clock::now() < round_end) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(round_end - Clock::now());
                    auto received = co_await receive_within(socket, 2048, (std::max)(left, std::chrono::milliseconds(1)), cancellation);
                    if (!received) co_return std::unexpected(received.error());
                    if (!*received) break;
                    UdpDatagram &datagram = **received;
                    // Any host on the LAN can answer SSDP; trust only the gateway (when we know it) or the host it speaks for.
                    if (!gateway.empty() && datagram.sender_host != gateway) continue;
                    const std::string text(reinterpret_cast<const char *>(datagram.data.data()), datagram.data.size());
                    if (text.rfind("HTTP/1.1 200", 0) != 0) continue;
                    std::optional<std::string> location;
                    std::istringstream lines(text);
                    for (std::string line; std::getline(lines, line);) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        const auto colon = line.find(':');
                        if (colon == std::string::npos) continue;
                        std::string name = line.substr(0, colon);
                        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (name == "location") location = trim(line.substr(colon + 1));
                    }
                    if (!location) continue;
                    auto where = parse_http_location(*location);
                    if (!where || where->host != datagram.sender_host) continue; // the description must live on the host that answered

                    auto description = co_await http->get(http_url(*where, where->path));
                    if (!description || description->status != 200) continue;

                    std::string_view xml = description->body;
                    std::size_t cursor = 0;
                    while (auto service = xml_text(xml, "service", cursor)) {
                        std::size_t inner = 0;
                        auto type = xml_text(*service, "serviceType", inner);
                        inner = 0;
                        auto control = xml_text(*service, "controlURL", inner);
                        if (!type || !control) continue;
                        const std::string service_type = trim(*type);
                        if (service_type.find(":WANIPConnection:") == std::string::npos && service_type.find(":WANPPPConnection:") == std::string::npos) continue;
                        std::string path = trim(*control);
                        if (path.rfind("http://", 0) == 0) {
                            auto absolute = parse_http_location(path);
                            if (!absolute || absolute->host != where->host) continue; // never follow the router to another machine
                            path = absolute->path;
                            where->port = absolute->port;
                        } else if (path.empty() || path.front() != '/') {
                            std::string directory = where->path.substr(0, where->path.rfind('/') + 1);
                            path = directory + path;
                        }
                        co_return std::expected<UpnpDevice, IoError>(UpnpDevice{*where, std::move(path), service_type});
                    }
                }
            }
            co_return std::unexpected(failure(IoErrorCode::TimedOut, "no UPnP gateway answered"));
        }

        struct SoapReply {
            unsigned status = 0;
            std::string body;
        };

        TaskHandle<std::expected<SoapReply, IoError>> soap_call(std::shared_ptr<HttpClient> http, HttpLocation where, std::string control_path,
                                                                 std::string service_type, std::string action, std::string arguments) {
            std::string envelope =
                "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + xml_escape(service_type) + "\">" +
                arguments + "</u:" + action + "></s:Body></s:Envelope>";
            auto reply = co_await http->post(http_url(where, control_path), std::move(envelope), "text/xml; charset=\"utf-8\"",
                                             {{"SOAPAction", "\"" + service_type + "#" + action + "\""}});
            if (!reply) co_return std::unexpected(reply.error());
            co_return std::expected<SoapReply, IoError>(SoapReply{reply->status, std::move(reply->body)});
        }

        std::string upnp_error_text(const SoapReply &reply) {
            std::size_t at = 0;
            std::string out = "UPnP: the router refused (HTTP " + std::to_string(reply.status) + ")";
            if (auto code = xml_text(reply.body, "errorCode", at)) {
                out += ", error " + trim(*code);
                at = 0;
                if (auto description = xml_text(reply.body, "errorDescription", at)) out += " (" + trim(*description) + ")";
            }
            return out;
        }

        TaskHandle<std::expected<PortMapping, IoError>> map_upnp(PortProtocol protocol, u16 internal_port, PortMapOptions options, std::string gateway,
                                                                  CancellationToken cancellation, std::uint32_t lifetime_seconds) {
            auto device = co_await discover_igd(options, gateway, cancellation);
            if (!device) co_return std::unexpected(device.error());
            auto http = make_igd_client(options.timeout);

            // The address the router should forward to: whatever source address we use to reach it.
            auto local = local_address_towards(device->location.host, device->location.port);
            if (!local) co_return std::unexpected(local.error());

            const u16 external_port = options.external_port != 0 ? options.external_port : internal_port;
            const char *protocol_name = protocol == PortProtocol::Udp ? "UDP" : "TCP";
            auto add = [&](std::uint32_t lease) {
                return soap_call(http, device->location, device->control_path, device->service_type, "AddPortMapping",
                                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external_port) + "</NewExternalPort><NewProtocol>" +
                                     protocol_name + "</NewProtocol><NewInternalPort>" + std::to_string(internal_port) +
                                     "</NewInternalPort><NewInternalClient>" + xml_escape(local->host) +
                                     "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>SturdyEngine</NewPortMappingDescription>"
                                     "<NewLeaseDuration>" + std::to_string(lease) + "</NewLeaseDuration>");
            };
            auto added = co_await add(lifetime_seconds);
            if (!added) co_return std::unexpected(added.error());
            std::uint32_t granted = lifetime_seconds;
            if (added->status != 200) {
                // 725 OnlyPermanentLeasesSupported: accept a permanent mapping rather than none (the caller must unmap it).
                std::size_t at = 0;
                const auto code = xml_text(added->body, "errorCode", at);
                if (!code || trim(*code) != "725") co_return std::unexpected(failure(IoErrorCode::PermissionDenied, upnp_error_text(*added)));
                added = co_await add(0);
                if (!added) co_return std::unexpected(added.error());
                if (added->status != 200) co_return std::unexpected(failure(IoErrorCode::PermissionDenied, upnp_error_text(*added)));
                granted = 0;
            }

            std::string external_host;
            if (auto address = co_await soap_call(http, device->location, device->control_path, device->service_type, "GetExternalIPAddress", "");
                address && address->status == 200) {
                std::size_t at = 0;
                if (auto text = xml_text(address->body, "NewExternalIPAddress", at)) external_host = trim(*text);
            }

            PortMapping mapping;
            mapping.protocol = protocol;
            mapping.internal_port = internal_port;
            mapping.external = {std::move(external_host), external_port};
            mapping.lifetime = std::chrono::seconds(granted);
            mapping.method = PortMapMethod::Upnp;
            mapping.gateway = device->location.host;
            mapping.upnp_control_url = http_url(device->location, device->control_path);
            mapping.upnp_service_type = device->service_type;
            co_return std::expected<PortMapping, IoError>(std::move(mapping));
        }

        std::uint32_t clamp_lifetime(std::chrono::seconds lifetime) {
            return static_cast<std::uint32_t>(std::clamp<long long>(lifetime.count(), 1, 7 * 24 * 3600));
        }

        // ---- the front door ---------------------------------------------------------------------------------------
        TaskHandle<std::expected<PortMapping, IoError>> map_port_task(PortProtocol protocol, u16 internal_port, PortMapOptions options,
                                                                      CancellationToken cancellation) {
            if (internal_port == 0) co_return std::unexpected(failure(IoErrorCode::InvalidArgument, "internal port must not be 0"));
            if (options.methods.empty()) co_return std::unexpected(failure(IoErrorCode::InvalidArgument, "no port mapping method enabled"));
            const std::uint32_t lifetime = clamp_lifetime(options.lifetime);

            std::string gateway;
            std::optional<IoError> gateway_problem;
            if (auto found = co_await gateway_address(options.gateway)) gateway = std::move(*found);
            else gateway_problem = found.error();

            std::string failures;
            for (PortMapMethod method : options.methods) {
                std::expected<PortMapping, IoError> result = std::unexpected(failure(IoErrorCode::Unknown, "unreachable"));
                if (method == PortMapMethod::Upnp) {
                    result = co_await map_upnp(protocol, internal_port, options, gateway, cancellation, lifetime);
                } else if (gateway.empty()) {
                    result = std::unexpected(*gateway_problem);
                } else if (method == PortMapMethod::Pcp) {
                    std::array<std::uint8_t, 12> nonce{};
                    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) co_return std::unexpected(failure(IoErrorCode::Unknown, "OpenSSL random generator failed"));
                    result = co_await map_pcp(protocol, internal_port, options, gateway, nonce, cancellation, lifetime);
                } else {
                    result = co_await map_natpmp(protocol, internal_port, options, gateway, cancellation, lifetime);
                }
                if (result) co_return result;
                failures += std::string(failures.empty() ? "" : "; ") + to_string(method) + ": " + result.error().message;
            }
            co_return std::unexpected(failure(IoErrorCode::Unsupported, "no port mapping method worked (" + failures + ")"));
        }

        TaskHandle<std::expected<void, IoError>> unmap_port_task(PortMapping mapping, PortMapOptions options, CancellationToken cancellation) {
            switch (mapping.method) {
                case PortMapMethod::Pcp: {
                    auto made = co_await socket_to_gateway(mapping.gateway);
                    if (!made) co_return std::unexpected(made.error());
                    auto socket = *made;
                    auto local = local_address_towards(mapping.gateway, options.gateway_port);
                    std::array<std::uint8_t, 4> client{};
                    if (!local || !parse_ipv4(local->host, client)) co_return std::unexpected(failure(IoErrorCode::Unsupported, "PCP: IPv4 gateways only"));
                    Bytes request;
                    put8(request, 2);
                    put8(request, 1);
                    put16(request, 0);
                    put32(request, 0); // lifetime 0 deletes
                    put_ipv4_mapped(request, client);
                    for (std::uint8_t byte : mapping.pcp_nonce) put8(request, byte);
                    put8(request, mapping.protocol == PortProtocol::Udp ? 17 : 6);
                    put8(request, 0);
                    put16(request, 0);
                    put16(request, mapping.internal_port);
                    put16(request, 0);
                    for (int i = 0; i < 16; ++i) put8(request, 0);
                    const auto nonce = mapping.pcp_nonce;
                    auto answered = co_await gateway_exchange<bool>(
                        socket, mapping.gateway, options.gateway_port, std::move(request), options.timeout,
                        [nonce](const Bytes &packet) -> std::expected<std::optional<bool>, IoError> {
                            if (packet.size() < 60 || (get8(packet, 1) & 0x7F) != 1 || std::memcmp(packet.data() + 24, nonce.data(), nonce.size()) != 0) {
                                return std::optional<bool>{};
                            }
                            if (const unsigned result = get8(packet, 3); result != 0) return std::unexpected(pcp_error(result));
                            return std::optional<bool>(true);
                        },
                        cancellation);
                    if (!answered) co_return std::unexpected(answered.error());
                    co_return std::expected<void, IoError>{};
                }
                case PortMapMethod::NatPmp: {
                    auto made = co_await socket_to_gateway(mapping.gateway);
                    if (!made) co_return std::unexpected(made.error());
                    auto socket = *made;
                    Bytes request;
                    put8(request, 0);
                    put8(request, mapping.protocol == PortProtocol::Udp ? 1 : 2);
                    put16(request, 0);
                    put16(request, mapping.internal_port);
                    put16(request, 0); // external port 0 + lifetime 0 = remove
                    put32(request, 0);
                    const unsigned expected_op = 128u + (mapping.protocol == PortProtocol::Udp ? 1u : 2u);
                    const u16 internal_port = mapping.internal_port;
                    auto answered = co_await gateway_exchange<bool>(
                        socket, mapping.gateway, options.gateway_port, std::move(request), options.timeout,
                        [expected_op, internal_port](const Bytes &packet) -> std::expected<std::optional<bool>, IoError> {
                            if (packet.size() < 16 || get8(packet, 1) != expected_op || get16(packet, 8) != internal_port) return std::optional<bool>{};
                            if (const unsigned result = get16(packet, 2); result != 0) return std::unexpected(natpmp_error(result));
                            return std::optional<bool>(true);
                        },
                        cancellation);
                    if (!answered) co_return std::unexpected(answered.error());
                    co_return std::expected<void, IoError>{};
                }
                case PortMapMethod::Upnp: {
                    auto where = parse_http_location(mapping.upnp_control_url);
                    if (!where) co_return std::unexpected(failure(IoErrorCode::InvalidArgument, "mapping has no UPnP control URL"));
                    auto http = make_igd_client(options.timeout);
                    auto reply = co_await soap_call(http, *where, where->path, mapping.upnp_service_type, "DeletePortMapping",
                                                    std::string("<NewRemoteHost></NewRemoteHost><NewExternalPort>") + std::to_string(mapping.external.port) +
                                                        "</NewExternalPort><NewProtocol>" + (mapping.protocol == PortProtocol::Udp ? "UDP" : "TCP") + "</NewProtocol>");
                    if (!reply) co_return std::unexpected(reply.error());
                    if (reply->status != 200) co_return std::unexpected(failure(IoErrorCode::PermissionDenied, upnp_error_text(*reply)));
                    co_return std::expected<void, IoError>{};
                }
            }
            co_return std::unexpected(failure(IoErrorCode::InvalidArgument, "unknown mapping method"));
        }

        // ---- hole punching --------------------------------------------------------------------------------------
        constexpr std::size_t kTagSize = 16;
        constexpr std::size_t kNonceSize = 8;
        constexpr std::string_view kMagic = "STNP";

        std::array<std::uint8_t, kTagSize> punch_tag(const std::string &secret, std::uint8_t type, const std::array<std::uint8_t, kNonceSize> &nonce) {
            std::array<std::uint8_t, 1 + kNonceSize> message{type};
            std::copy(nonce.begin(), nonce.end(), message.begin() + 1);
            unsigned char digest[32];
            unsigned length = 0;
            HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()), message.data(), message.size(), digest, &length);
            std::array<std::uint8_t, kTagSize> tag{};
            std::copy_n(digest, kTagSize, tag.begin());
            return tag;
        }

        Bytes punch_packet(const std::string &secret, std::uint8_t type, const std::array<std::uint8_t, kNonceSize> &nonce) {
            Bytes out;
            for (char c : kMagic) put8(out, static_cast<unsigned char>(c));
            put8(out, type);
            for (std::uint8_t byte : nonce) put8(out, byte);
            for (std::uint8_t byte : punch_tag(secret, type, nonce)) put8(out, byte);
            return out;
        }

        /// Verifies magic + HMAC; fills type and nonce.
        bool punch_parse(const std::string &secret, const Bytes &packet, std::uint8_t &type, std::array<std::uint8_t, kNonceSize> &nonce) {
            if (packet.size() != kMagic.size() + 1 + kNonceSize + kTagSize) return false;
            if (std::memcmp(packet.data(), kMagic.data(), kMagic.size()) != 0) return false;
            type = static_cast<std::uint8_t>(get8(packet, kMagic.size()));
            if (type != 1 && type != 2) return false;
            for (std::size_t i = 0; i < kNonceSize; ++i) nonce[i] = static_cast<std::uint8_t>(get8(packet, kMagic.size() + 1 + i));
            const auto expected = punch_tag(secret, type, nonce);
            return CRYPTO_memcmp(expected.data(), packet.data() + kMagic.size() + 1 + kNonceSize, kTagSize) == 0;
        }

        TaskHandle<std::expected<SocketAddress, IoError>> punch_task(UdpSocket *socket, std::vector<SocketAddress> candidates, std::string secret,
                                                                     HolePunchOptions options, CancellationToken cancellation) {
            if (candidates.empty() || candidates.size() > 16) co_return std::unexpected(failure(IoErrorCode::InvalidArgument, "give 1 to 16 candidate addresses"));
            if (secret.empty()) co_return std::unexpected(failure(IoErrorCode::InvalidArgument, "a shared secret is required"));
            std::array<std::uint8_t, kNonceSize> mine{};
            if (RAND_bytes(mine.data(), static_cast<int>(mine.size())) != 1) co_return std::unexpected(failure(IoErrorCode::Unknown, "OpenSSL random generator failed"));
            const Bytes probe = punch_packet(secret, 1, mine);
            const auto deadline = Clock::now() + options.timeout;

            std::optional<SocketAddress> confirmed;
            auto answer_probes_until = [&](Clock::time_point until, bool stop_on_ack) -> TaskHandle<std::expected<bool, IoError>> {
                while (Clock::now() < until) {
                    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now());
                    auto received = co_await receive_within(*socket, 256, (std::max)(left, std::chrono::milliseconds(1)), cancellation);
                    if (!received) co_return std::unexpected(received.error());
                    if (!*received) break;
                    UdpDatagram &datagram = **received;
                    std::uint8_t type = 0;
                    std::array<std::uint8_t, kNonceSize> nonce{};
                    if (!punch_parse(secret, datagram.data, type, nonce)) continue;
                    if (type == 1) {
                        // The peer's probe: prove we know the secret by echoing its nonce.
                        (void)(co_await socket->send_to(datagram.sender_host, datagram.sender_port, punch_packet(secret, 2, nonce)));
                    } else if (nonce == mine && !confirmed) {
                        confirmed = SocketAddress{datagram.sender_host, datagram.sender_port};
                        if (stop_on_ack) co_return std::expected<bool, IoError>(true);
                    }
                }
                co_return std::expected<bool, IoError>(false);
            };

            while (Clock::now() < deadline && !confirmed) {
                for (const SocketAddress &candidate : candidates) {
                    (void)(co_await socket->send_to(candidate.host, candidate.port, probe)); // an unroutable candidate is not fatal
                }
                auto round = co_await answer_probes_until((std::min)(Clock::now() + options.interval, deadline), true);
                if (!round) co_return std::unexpected(round.error());
            }
            if (!confirmed) co_return std::unexpected(failure(IoErrorCode::TimedOut, "no hole could be punched (the peer did not answer; try a relay)"));

            // The other side may still be waiting for the acknowledgement of *its* probe: keep answering for a moment.
            auto linger = co_await answer_probes_until(Clock::now() + options.interval * 2 + std::chrono::milliseconds(250), false);
            if (!linger) co_return std::unexpected(linger.error());
            co_return std::expected<SocketAddress, IoError>(*confirmed);
        }

    } // namespace

    std::expected<std::string, IoError> default_gateway() {
#if defined(__linux__)
        std::ifstream routes("/proc/net/route");
        if (!routes) return std::unexpected(failure(IoErrorCode::Unsupported, "cannot read the routing table; set PortMapOptions::gateway"));
        std::string line;
        std::getline(routes, line); // header
        while (std::getline(routes, line)) {
            std::istringstream fields(line);
            std::string interface_name, destination, gateway, flags;
            if (!(fields >> interface_name >> destination >> gateway >> flags)) continue;
            if (destination != "00000000") continue;
            unsigned long flag_bits = 0;
            unsigned long address = 0;
            try {
                flag_bits = std::stoul(flags, nullptr, 16);
                address = std::stoul(gateway, nullptr, 16);
            } catch (...) {
                continue;
            }
            if ((flag_bits & 0x2) == 0 || address == 0) continue; // RTF_GATEWAY
            return format_ipv4({static_cast<std::uint8_t>(address & 0xFF), static_cast<std::uint8_t>((address >> 8) & 0xFF),
                                static_cast<std::uint8_t>((address >> 16) & 0xFF), static_cast<std::uint8_t>((address >> 24) & 0xFF)});
        }
        return std::unexpected(failure(IoErrorCode::NotFound, "no default route; set PortMapOptions::gateway"));
#else
        return std::unexpected(failure(IoErrorCode::Unsupported, "default gateway discovery is only implemented on Linux; set PortMapOptions::gateway"));
#endif
    }

    TaskHandle<std::expected<PortMapping, IoError>> map_port(PortProtocol protocol, u16 internal_port, PortMapOptions options, CancellationToken cancellation) {
        return map_port_task(protocol, internal_port, std::move(options), std::move(cancellation));
    }

    TaskHandle<std::expected<void, IoError>> unmap_port(PortMapping mapping, PortMapOptions options, CancellationToken cancellation) {
        return unmap_port_task(std::move(mapping), std::move(options), std::move(cancellation));
    }

    TaskHandle<std::expected<SocketAddress, IoError>> punch_udp_hole(UdpSocket &socket, std::vector<SocketAddress> candidates, std::string shared_secret,
                                                                     HolePunchOptions options, CancellationToken cancellation) {
        return punch_task(&socket, std::move(candidates), std::move(shared_secret), options, std::move(cancellation));
    }

} // namespace SFT::Async

#else

namespace SFT::Async {
    const char *to_string(PortMapMethod method) noexcept {
        switch (method) {
            case PortMapMethod::Pcp: return "PCP";
            case PortMapMethod::NatPmp: return "NAT-PMP";
            case PortMapMethod::Upnp: return "UPnP";
        }
        return "?";
    }
    std::expected<std::string, IoError> default_gateway() { return std::unexpected(IoError{IoErrorCode::Unsupported, "NAT traversal is not available on Web"}); }
    TaskHandle<std::expected<PortMapping, IoError>> map_port(PortProtocol, u16, PortMapOptions, CancellationToken) {
        Promise<std::expected<PortMapping, IoError>> promise;
        (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "NAT traversal is not available on Web"}));
        return promise.handle();
    }
    TaskHandle<std::expected<void, IoError>> unmap_port(PortMapping, PortMapOptions, CancellationToken) {
        Promise<std::expected<void, IoError>> promise;
        (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "NAT traversal is not available on Web"}));
        return promise.handle();
    }
    TaskHandle<std::expected<SocketAddress, IoError>> punch_udp_hole(UdpSocket &, std::vector<SocketAddress>, std::string, HolePunchOptions, CancellationToken) {
        Promise<std::expected<SocketAddress, IoError>> promise;
        (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "NAT traversal is not available on Web"}));
        return promise.handle();
    }
} // namespace SFT::Async

#endif
