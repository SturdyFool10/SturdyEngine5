// Port mapping (PCP, NAT-PMP, UPnP IGD) against in-process fake routers, and UDP hole punching on loopback.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("NatTraversalTest FAILED: {}", what);
            ++failures;
        }
    }

    using Bytes = std::vector<std::byte>;

    unsigned at8(const Bytes &b, std::size_t i) { return std::to_integer<unsigned>(b[i]); }
    unsigned at16(const Bytes &b, std::size_t i) { return (at8(b, i) << 8) | at8(b, i + 1); }
    void add8(Bytes &b, unsigned v) { b.push_back(static_cast<std::byte>(v)); }
    void add16(Bytes &b, unsigned v) { add8(b, v >> 8); add8(b, v & 255); }
    void add32(Bytes &b, unsigned v) { add16(b, v >> 16); add16(b, v & 0xFFFF); }

    /// A fake router: a UDP socket on loopback and a thread that answers each datagram with whatever `respond` returns
    /// (any number of replies, so tests can inject decoys first).
    class FakeUdpService {
      public:
        using Handler = std::function<std::vector<Bytes>(const Bytes &request)>;

        explicit FakeUdpService(Handler handler) : handler_(std::move(handler)) {
            auto bound = UdpSocket::bind("127.0.0.1", 0).wait();
            socket_ = std::move(*bound);
            port_ = socket_.local_port().value();
            thread_ = std::thread([this] {
                for (;;) {
                    try {
                        auto datagram = socket_.receive_from(2048, stop_.token()).wait();
                        if (!datagram) return;
                        ++received_;
                        for (Bytes &reply : handler_(datagram->data)) (void)socket_.send_to(datagram->sender_host, datagram->sender_port, std::move(reply)).wait();
                    } catch (...) {
                        return;
                    }
                }
            });
        }
        ~FakeUdpService() {
            (void)stop_.cancel();
            thread_.join();
        }
        [[nodiscard]] u16 port() const { return port_; }
        [[nodiscard]] int received() const { return received_; }

      private:
        Handler handler_;
        UdpSocket socket_;
        u16 port_ = 0;
        CancellationSource stop_;
        std::atomic<int> received_{0};
        std::thread thread_;
    };

    PortMapOptions options_for(const FakeUdpService &service, std::vector<PortMapMethod> methods) {
        PortMapOptions options;
        options.gateway = "127.0.0.1";
        options.gateway_port = service.port();
        options.methods = std::move(methods);
        options.timeout = 1500ms;
        return options;
    }

    // ---- NAT-PMP fake ----------------------------------------------------------------------------------------
    struct NatPmpRouter {
        std::mutex lock;
        unsigned last_lifetime = 99999;
        unsigned last_external_request = 0;
        unsigned refuse_with = 0; ///< non-zero result code for mapping requests
        FakeUdpService service{[this](const Bytes &request) -> std::vector<Bytes> {
            std::lock_guard guard(lock);
            if (request.size() < 2) return {};
            if (at8(request, 0) != 0) { // PCP request: answer like a NAT-PMP-only router
                Bytes reply;
                add8(reply, 0);
                add8(reply, 128 | at8(request, 1));
                add16(reply, 1);
                add32(reply, 1000);
                return {reply};
            }
            Bytes reply;
            add8(reply, 0);
            add8(reply, 128 | at8(request, 1));
            if (at8(request, 1) == 0) {
                add16(reply, 0);
                add32(reply, 1234);
                add8(reply, 203); add8(reply, 0); add8(reply, 113); add8(reply, 9);
                return {reply};
            }
            if (request.size() < 12) return {};
            add16(reply, refuse_with);
            add32(reply, 1234);
            add16(reply, at16(request, 4)); // internal port
            last_external_request = at16(request, 6);
            const unsigned requested_lifetime = (at16(request, 8) << 16) | at16(request, 10);
            last_lifetime = requested_lifetime;
            add16(reply, requested_lifetime == 0 ? 0 : at16(request, 4) + 1000); // router prefers port+1000
            add32(reply, requested_lifetime == 0 ? 0 : (std::min)(requested_lifetime, 1800u));
            return {reply};
        }};
    };

    // ---- PCP fake --------------------------------------------------------------------------------------------
    struct PcpRouter {
        std::mutex lock;
        unsigned result = 0;
        bool decoy_first = false;
        unsigned last_lifetime = 99999;
        FakeUdpService service{[this](const Bytes &request) -> std::vector<Bytes> {
            std::lock_guard guard(lock);
            if (request.size() < 60 || at8(request, 0) != 2) return {};
            Bytes reply;
            add8(reply, 2);
            add8(reply, 0x81);
            add8(reply, 0);
            add8(reply, result);
            const unsigned lifetime = (at16(request, 4) << 16) | at16(request, 6);
            last_lifetime = lifetime;
            add32(reply, lifetime == 0 ? 0 : (std::min)(lifetime, 7200u));
            add32(reply, 5555);
            for (int i = 0; i < 12; ++i) add8(reply, 0);
            for (std::size_t i = 24; i < 36; ++i) add8(reply, at8(request, i)); // nonce
            add8(reply, at8(request, 36));                                       // protocol
            add8(reply, 0); add16(reply, 0);
            add16(reply, at16(request, 40));                                     // internal port
            add16(reply, 40000);                                                 // assigned external port
            for (int i = 0; i < 10; ++i) add8(reply, 0);                         // ::ffff:198.51.100.7
            add16(reply, 0xFFFF);
            add8(reply, 198); add8(reply, 51); add8(reply, 100); add8(reply, 7);
            if (!decoy_first) return {reply};
            Bytes decoy = reply;
            decoy[24] = std::byte{0xEE}; // someone else's nonce: must be ignored
            return {decoy, reply};
        }};
    };

    // ---- UPnP fake ---------------------------------------------------------------------------------------------
    struct UpnpRouter {
        std::mutex lock;
        std::vector<std::string> actions;
        std::string last_add_body;
        bool permanent_only = false;
        bool absolute_control_elsewhere = false;
        std::string ssdp_location_host = "127.0.0.1"; ///< what LOCATION claims
        Router router;
        std::unique_ptr<HttpServer> http;
        std::unique_ptr<FakeUdpService> ssdp;

        UpnpRouter() {
            router.get("/desc.xml", [this](const RouteRequest &) {
                const std::string control = absolute_control_elsewhere ? "http://10.99.99.99:5000/ctl" : "/ctl";
                return text_response(200, "<root><device><deviceList><device><serviceList>"
                                          "<service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/nope</controlURL></service>"
                                          "<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>" + control + "</controlURL></service>"
                                          "</serviceList></device></deviceList></device></root>");
            });
            router.post("/ctl", [this](const RouteRequest &r) {
                std::lock_guard guard(lock);
                const auto action = r.header("soapaction").value_or("");
                actions.push_back(std::string(action));
                if (action.find("#AddPortMapping") != std::string::npos) {
                    last_add_body = r.http.body;
                    const bool permanent_request = r.http.body.find("<NewLeaseDuration>0</NewLeaseDuration>") != std::string::npos;
                    if (permanent_only && !permanent_request) {
                        return text_response(500, "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>725</errorCode>"
                                                  "<errorDescription>OnlyPermanentLeasesSupported</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>");
                    }
                    return text_response(200, "<s:Envelope><s:Body><u:AddPortMappingResponse/></s:Body></s:Envelope>");
                }
                if (action.find("#GetExternalIPAddress") != std::string::npos) {
                    return text_response(200, "<s:Envelope><s:Body><u:GetExternalIPAddressResponse><NewExternalIPAddress>192.0.2.77</NewExternalIPAddress>"
                                              "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>");
                }
                if (action.find("#DeletePortMapping") != std::string::npos) return text_response(200, "<ok/>");
                return text_response(500, "<bad/>");
            });
            HttpServerOptions options;
            options.bind_address = "127.0.0.1";
            options.handler_threads = 2;
            http = std::make_unique<HttpServer>(options, router.handler());
            if (!http->start()) std::abort();
            const u16 http_port = http->local_port().value();
            ssdp = std::make_unique<FakeUdpService>([this, http_port](const Bytes &request) -> std::vector<Bytes> {
                const std::string text(reinterpret_cast<const char *>(request.data()), request.size());
                if (text.rfind("M-SEARCH", 0) != 0) return {};
                const std::string reply = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nLOCATION: http://" +
                                          ssdp_location_host + ":" + std::to_string(http_port) + "/desc.xml\r\n\r\n";
                Bytes out(reply.size());
                std::memcpy(out.data(), reply.data(), reply.size());
                return {out};
            });
        }
        ~UpnpRouter() { http->stop(); }
    };

} // namespace

int main() {
    Scheduler::initialize(2);

    // ---- default gateway discovery doesn't crash and gives a numeric answer when it works ----
    {
        auto gateway = default_gateway();
        Foundation::log_info("NatTraversalTest: default gateway: {}", gateway ? *gateway : gateway.error().message);
        check(!gateway || gateway->find('.') != std::string::npos, "default_gateway returns a dotted address when it finds one");
    }

    // ---- NAT-PMP ----
    {
        NatPmpRouter router;
        auto options = options_for(router.service, {PortMapMethod::NatPmp});
        options.lifetime = 7200s;
        auto mapping = map_port(PortProtocol::Udp, 7000, options).wait();
        check(mapping.has_value(), "NAT-PMP mapping succeeds");
        if (mapping) {
            check(mapping->method == PortMapMethod::NatPmp, "...reports the method");
            check(mapping->external.host == "203.0.113.9" && mapping->external.port == 8000, "...public address from op 0, port chosen by the router");
            check(mapping->lifetime == 1800s, "...lifetime is what the router granted, not what we asked");
            check(router.last_external_request == 7000, "...asks for the same external port by default");
            auto removed = unmap_port(*mapping, options).wait();
            check(removed.has_value(), "NAT-PMP unmap succeeds");
            check(router.last_lifetime == 0, "...by sending lifetime 0");
        }
        router.refuse_with = 2;
        auto refused = map_port(PortProtocol::Tcp, 7001, options).wait();
        check(!refused && refused.error().message.find("not authorized") != std::string::npos, "a refusal is reported with the router's reason");
    }

    // ---- PCP ----
    {
        PcpRouter router;
        auto options = options_for(router.service, {PortMapMethod::Pcp});
        router.decoy_first = true;
        auto mapping = map_port(PortProtocol::Tcp, 7100, options).wait();
        check(mapping.has_value(), "PCP mapping succeeds (and ignores an answer with the wrong nonce)");
        if (mapping) {
            check(mapping->method == PortMapMethod::Pcp && mapping->external.host == "198.51.100.7" && mapping->external.port == 40000, "...public address and port from the answer");
            check(mapping->lifetime == 3600s, "...lifetime");
            check(unmap_port(*mapping, options).wait().has_value() && router.last_lifetime == 0, "PCP unmap sends lifetime 0 with the same nonce");
        }
        router.decoy_first = false;
        router.result = 2;
        auto refused = map_port(PortProtocol::Tcp, 7101, options).wait();
        check(!refused && refused.error().code == IoErrorCode::Unsupported && refused.error().message.find("not authorized") != std::string::npos, "PCP refusal is reported");
    }

    // ---- fallback: PCP -> NAT-PMP when the router only speaks NAT-PMP ----
    {
        NatPmpRouter router;
        auto options = options_for(router.service, {PortMapMethod::Pcp, PortMapMethod::NatPmp});
        const auto start = std::chrono::steady_clock::now();
        auto mapping = map_port(PortProtocol::Udp, 7200, options).wait();
        check(mapping && mapping->method == PortMapMethod::NatPmp, "falls back from PCP to NAT-PMP");
        check(std::chrono::steady_clock::now() - start < 1s, "...immediately, because the router said 'unsupported version' (no timeout wait)");
    }

    // ---- nobody home: every method fails with a combined message, within the timeout ----
    {
        FakeUdpService silent([](const Bytes &) { return std::vector<Bytes>{}; });
        auto options = options_for(silent, {PortMapMethod::Pcp, PortMapMethod::NatPmp});
        options.timeout = 400ms;
        const auto start = std::chrono::steady_clock::now();
        auto mapping = map_port(PortProtocol::Tcp, 7300, options).wait();
        check(!mapping && mapping.error().message.find("PCP") != std::string::npos && mapping.error().message.find("NAT-PMP") != std::string::npos, "silence reports both methods");
        check(std::chrono::steady_clock::now() - start < 3s, "...and respects the per-method timeout");
        check(!map_port(PortProtocol::Tcp, 0, options).wait().has_value(), "internal port 0 is rejected");
    }

    // ---- UPnP ----
    {
        UpnpRouter router;
        PortMapOptions options;
        options.gateway = "127.0.0.1";
        options.methods = {PortMapMethod::Upnp};
        options.ssdp_host = "127.0.0.1";
        options.ssdp_port = router.ssdp->port();
        options.timeout = 2000ms;
        options.external_port = 9000;
        auto mapping = map_port(PortProtocol::Tcp, 7400, options).wait();
        check(mapping.has_value(), "UPnP mapping succeeds");
        if (mapping) {
            check(mapping->method == PortMapMethod::Upnp && mapping->external.host == "192.0.2.77" && mapping->external.port == 9000, "...external address from GetExternalIPAddress");
            check(mapping->lifetime == 3600s, "...lifetime");
            {
                std::lock_guard guard(router.lock);
                check(router.last_add_body.find("<NewInternalPort>7400</NewInternalPort>") != std::string::npos &&
                          router.last_add_body.find("<NewInternalClient>127.0.0.1</NewInternalClient>") != std::string::npos &&
                          router.last_add_body.find("<NewProtocol>TCP</NewProtocol>") != std::string::npos,
                      "...SOAP request names the right ports, protocol and our address");
            }
            check(unmap_port(*mapping, options).wait().has_value(), "UPnP unmap succeeds");
            std::lock_guard guard(router.lock);
            check(!router.actions.empty() && router.actions.back().find("#DeletePortMapping") != std::string::npos, "...via DeletePortMapping");
        }

        router.permanent_only = true;
        auto permanent = map_port(PortProtocol::Udp, 7401, options).wait();
        check(permanent && permanent->lifetime == 0s, "error 725 falls back to a permanent lease, which is reported as lifetime 0");

        // A LOCATION that names another host than the one that answered is ignored (no SSRF through SSDP).
        router.ssdp_location_host = "10.99.99.99";
        auto spoofed = options;
        spoofed.timeout = 600ms;
        check(!map_port(PortProtocol::Tcp, 7402, spoofed).wait().has_value(), "a LOCATION on a different host than the SSDP answerer is refused");
        router.ssdp_location_host = "127.0.0.1";

        // A control URL that points at another machine is skipped.
        router.absolute_control_elsewhere = true;
        check(!map_port(PortProtocol::Tcp, 7403, spoofed).wait().has_value(), "an absolute control URL on another host is refused");
        router.absolute_control_elsewhere = false;
    }

    // ---- hole punching ----
    {
        auto a = UdpSocket::bind("127.0.0.1", 0).wait();
        auto b = UdpSocket::bind("127.0.0.1", 0).wait();
        check(a && b, "bind punch sockets");
        const SocketAddress a_address{"127.0.0.1", a->local_port().value()};
        const SocketAddress b_address{"127.0.0.1", b->local_port().value()};

        HolePunchOptions fast;
        fast.interval = 50ms;
        fast.timeout = 4000ms;
        auto side_a = punch_udp_hole(*a, {b_address}, "correct horse battery staple", fast);
        auto side_b = punch_udp_hole(*b, {{"127.0.0.1", 1}, a_address}, "correct horse battery staple", fast); // one dead candidate is harmless
        auto result_a = side_a.wait();
        auto result_b = side_b.wait();
        check(result_a && result_a->port == b_address.port, "side A learns B's address");
        check(result_b && result_b->port == a_address.port, "side B learns A's address");

        // The sockets are usable for ordinary traffic afterwards.
        Bytes hello(5, std::byte{'h'});
        check(a->send_to(b_address.host, b_address.port, hello).wait().has_value(), "send over the punched path");
        auto got = b->receive_from(100, CancellationToken{}).wait();
        check(got && got->data == hello, "...and receive it");

        // Different secrets: neither side can complete.
        auto c = UdpSocket::bind("127.0.0.1", 0).wait();
        auto d = UdpSocket::bind("127.0.0.1", 0).wait();
        HolePunchOptions brief;
        brief.interval = 50ms;
        brief.timeout = 600ms;
        auto side_c = punch_udp_hole(*c, {{"127.0.0.1", d->local_port().value()}}, "secret one", brief);
        auto side_d = punch_udp_hole(*d, {{"127.0.0.1", c->local_port().value()}}, "secret two", brief);
        auto result_c = side_c.wait();
        auto result_d = side_d.wait();
        check(!result_c && result_c.error().code == IoErrorCode::TimedOut && !result_d && result_d.error().code == IoErrorCode::TimedOut, "mismatched secrets time out on both sides");

        // Input validation.
        check(!punch_udp_hole(*a, {}, "x").wait().has_value(), "no candidates is rejected");
        check(!punch_udp_hole(*a, {b_address}, "").wait().has_value(), "an empty secret is rejected");

        // Cancellation ends it promptly.
        CancellationSource stop;
        auto e = UdpSocket::bind("127.0.0.1", 0).wait();
        auto pending = punch_udp_hole(*e, {{"127.0.0.1", 9}}, "secret", fast, stop.token());
        std::this_thread::sleep_for(200ms);
        (void)stop.cancel();
        bool cancelled = false;
        try {
            (void)pending.wait();
        } catch (const IoOperationCancelled &) {
            cancelled = true;
        }
        check(cancelled, "cancelling a punch attempt ends it");
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
