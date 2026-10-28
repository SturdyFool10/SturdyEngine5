#include <Async/Stun.hpp>

#include <Async/Promise.hpp>
#include <Async/Timer.hpp>

#if !defined(STURDY_PLATFORM_WEB)
#include <openssl/rand.h>
#endif

#include <array>
#include <cstring>
#include <memory>

namespace SFT::Async {

    namespace {

        constexpr std::uint32_t kMagicCookie = 0x2112A442;
        using Transaction = std::array<std::uint8_t, 12>;

        std::uint16_t read16(const std::byte *p) { return static_cast<std::uint16_t>((std::to_integer<unsigned>(p[0]) << 8) | std::to_integer<unsigned>(p[1])); }

        std::string format_v4(const std::uint8_t *octets) {
            return std::to_string(octets[0]) + "." + std::to_string(octets[1]) + "." + std::to_string(octets[2]) + "." + std::to_string(octets[3]);
        }

        std::string format_v6(const std::uint8_t *octets) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out;
            for (int group = 0; group < 8; ++group) {
                if (group) out += ':';
                const unsigned value = (static_cast<unsigned>(octets[group * 2]) << 8) | octets[group * 2 + 1];
                char text[5] = {digits[(value >> 12) & 15], digits[(value >> 8) & 15], digits[(value >> 4) & 15], digits[value & 15], 0};
                out += text;
            }
            return out;
        }

        /// Parses a Binding success response; returns the reflexive address or nullopt if it is not ours / not valid.
        std::optional<SocketAddress> parse_response(const std::vector<std::byte> &packet, const Transaction &transaction) {
            if (packet.size() < 20 || read16(packet.data()) != 0x0101) return std::nullopt;
            const std::uint16_t length = read16(packet.data() + 2);
            if (20u + length > packet.size() || (length % 4) != 0) return std::nullopt;
            const std::uint32_t cookie = (std::to_integer<std::uint32_t>(packet[4]) << 24) | (std::to_integer<std::uint32_t>(packet[5]) << 16) |
                                         (std::to_integer<std::uint32_t>(packet[6]) << 8) | std::to_integer<std::uint32_t>(packet[7]);
            if (cookie != kMagicCookie) return std::nullopt;
            for (std::size_t i = 0; i < transaction.size(); ++i) {
                if (std::to_integer<std::uint8_t>(packet[8 + i]) != transaction[i]) return std::nullopt; // someone else's answer
            }
            std::optional<SocketAddress> mapped;
            std::size_t offset = 20;
            while (offset + 4 <= 20u + length) {
                const std::uint16_t type = read16(packet.data() + offset);
                const std::uint16_t size = read16(packet.data() + offset + 2);
                const std::size_t value = offset + 4;
                if (value + size > 20u + length) return std::nullopt;
                const bool xor_mapped = type == 0x0020;
                if ((xor_mapped || type == 0x0001) && size >= 8 && std::to_integer<unsigned>(packet[value + 1]) <= 2) {
                    const unsigned family = std::to_integer<unsigned>(packet[value + 1]);
                    std::uint16_t port = read16(packet.data() + value + 2);
                    std::array<std::uint8_t, 16> address{};
                    const std::size_t address_size = family == 1 ? 4 : 16;
                    if (size >= 4 + address_size) {
                        for (std::size_t i = 0; i < address_size; ++i) address[i] = std::to_integer<std::uint8_t>(packet[value + 4 + i]);
                        if (xor_mapped) {
                            port ^= static_cast<std::uint16_t>(kMagicCookie >> 16);
                            std::array<std::uint8_t, 16> mask{static_cast<std::uint8_t>(kMagicCookie >> 24), static_cast<std::uint8_t>(kMagicCookie >> 16),
                                                              static_cast<std::uint8_t>(kMagicCookie >> 8), static_cast<std::uint8_t>(kMagicCookie)};
                            for (std::size_t i = 0; i < transaction.size(); ++i) mask[4 + i] = transaction[i];
                            for (std::size_t i = 0; i < address_size; ++i) address[i] ^= mask[i];
                        }
                        SocketAddress result{family == 1 ? format_v4(address.data()) : format_v6(address.data()), port};
                        if (xor_mapped) return result; // preferred form
                        mapped = std::move(result);
                    }
                }
                offset = value + ((size + 3u) & ~3u);
            }
            return mapped;
        }

        struct Query : std::enable_shared_from_this<Query> {
            UdpSocket socket;
            Transaction transaction{};
            std::string host;
            u16 port = 0;
            std::chrono::steady_clock::time_point deadline;
            Promise<std::expected<SocketAddress, IoError>> promise;
            CancellationSource stop; // ends the receive loop
            bool finished = false;
            Mutex<bool> guard;
        };

    } // namespace

#if !defined(STURDY_PLATFORM_WEB)

    TaskHandle<std::expected<SocketAddress, IoError>> stun_query(std::string server_host, u16 server_port, std::chrono::milliseconds timeout,
                                                                 CancellationToken cancellation) {
        auto query = std::make_shared<Query>();
        query->host = std::move(server_host);
        query->port = server_port;
        query->deadline = Clock::now() + timeout;
        auto handle = query->promise.handle();
        if (RAND_bytes(query->transaction.data(), static_cast<int>(query->transaction.size())) != 1) {
            (void)query->promise.set_value(std::unexpected(IoError{IoErrorCode::Unknown, "OpenSSL random generator failed"}));
            return handle;
        }
        auto bound = UdpSocket::bind("", 0).wait();
        if (!bound) {
            (void)query->promise.set_value(std::unexpected(bound.error()));
            return handle;
        }
        query->socket = std::move(*bound);

        auto finish = [query](std::expected<SocketAddress, IoError> result) {
            {
                auto lock = query->guard.lock();
                if (*lock) return;
                *lock = true;
            }
            (void)query->stop.cancel();
            query->socket.close();
            (void)query->promise.set_value(std::move(result));
        };
        std::vector<std::byte> request(20, std::byte{0});
        request[1] = std::byte{0x01}; // Binding request, no attributes
        request[4] = std::byte{0x21};
        request[5] = std::byte{0x12};
        request[6] = std::byte{0xA4};
        request[7] = std::byte{0x42};
        for (std::size_t i = 0; i < query->transaction.size(); ++i) request[8 + i] = static_cast<std::byte>(query->transaction[i]);

        // Receive loop: ignore stray datagrams until the matching response arrives.
        auto cancel_registration = std::make_shared<CancellationRegistration>(cancellation.on_cancel([query] {
            auto lock = query->guard.lock();
            if (*lock) return;
            *lock = true;
            (void)query->stop.cancel();
            query->socket.close();
            (void)query->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
        }));
        struct Loop {
            static void receive(const std::shared_ptr<Query> &q, std::function<void(std::expected<SocketAddress, IoError>)> done,
                                std::shared_ptr<CancellationRegistration> keep_alive) {
                auto pending = q->socket.receive_from(1500, q->stop.token());
                pending.on_done([q, pending, done, keep_alive]() mutable {
                    try {
                        auto datagram = pending.wait();
                        if (!datagram) return done(std::unexpected(datagram.error()));
                        if (auto address = parse_response(datagram->data, q->transaction)) return done(*address);
                        receive(q, done, keep_alive);
                    } catch (...) {
                        // cancelled by finish()/the caller: whoever cancelled has already settled the promise
                    }
                });
            }
        };
        Loop::receive(query, finish, cancel_registration);

        // Send with exponential back-off (RFC 5389 section 7.2.1) until the deadline.
        struct Sender {
            static void send(const std::shared_ptr<Query> &q, std::vector<std::byte> packet, std::chrono::milliseconds wait,
                             std::function<void(std::expected<SocketAddress, IoError>)> done) {
                auto sent = q->socket.send_to(q->host, q->port, packet);
                sent.on_done([q, sent, packet, wait, done]() mutable {
                    auto result = sent.wait();
                    if (!result) return done(std::unexpected(result.error()));
                    if (Clock::now() + wait > q->deadline) {
                        auto last = q->deadline > Clock::now() ? std::chrono::duration_cast<std::chrono::milliseconds>(q->deadline - Clock::now()) : std::chrono::milliseconds(0);
                        auto timer = sleep_for(last, q->stop.token());
                        timer.on_done([q, timer, done]() mutable {
                            try {
                                timer.wait();
                            } catch (...) {
                                return;
                            }
                            done(std::unexpected(IoError{IoErrorCode::TimedOut, "STUN server did not answer"}));
                        });
                        return;
                    }
                    auto timer = sleep_for(wait, q->stop.token());
                    timer.on_done([q, timer, packet, wait, done]() mutable {
                        try {
                            timer.wait();
                        } catch (...) {
                            return; // finished or cancelled meanwhile
                        }
                        send(q, packet, wait * 2, done);
                    });
                });
            }
        };
        Sender::send(query, std::move(request), std::chrono::milliseconds(500), finish);
        return handle;
    }

#else

    TaskHandle<std::expected<SocketAddress, IoError>> stun_query(std::string, u16, std::chrono::milliseconds, CancellationToken) {
        Promise<std::expected<SocketAddress, IoError>> promise;
        (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "STUN needs UDP sockets, unavailable on Web"}));
        return promise.handle();
    }

#endif

} // namespace SFT::Async
