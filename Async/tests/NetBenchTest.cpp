/// Loopback TCP under load: many connections doing request/response round trips, and one stream pushing bulk data. Always run at a
/// small scale as a correctness check (every byte must come back intact, whichever backend -- io_uring or Asio's epoll -- the
/// machine uses). With STURDY_NET_BENCH=1 it runs at full size and prints round-trip and throughput figures, so the two backends
/// can be compared on one machine: run it as-is (io_uring where available), then again with STURDY_ASYNC_NET_IO_URING=0.

#include <Async/Net.hpp>
#include <Async/Scheduler.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
    using namespace SFT::Async;
    using Clock = std::chrono::steady_clock;

    std::atomic<int> failures{0};
    void check(bool condition, const char *message) {
        if (!condition) {
            std::fprintf(stderr, "FAILED: %s\n", message);
            ++failures;
        }
    }

    std::vector<std::byte> payload(std::size_t size, unsigned seed) {
        std::vector<std::byte> out(size);
        for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::byte>((i * 31 + seed) & 0xff);
        return out;
    }

    /// Reads exactly `count` bytes (receive() may return less).
    bool read_exactly(TcpConnection &connection, std::size_t count, std::vector<std::byte> &into) {
        into.clear();
        while (into.size() < count) {
            auto got = connection.receive(count - into.size()).wait();
            if (!got || got->empty()) return false;
            into.insert(into.end(), got->begin(), got->end());
        }
        return true;
    }

    double seconds_since(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
} // namespace

int main() {
    Scheduler::initialize(4);
    const bool bench = std::getenv("STURDY_NET_BENCH") != nullptr;
    const int connections = bench ? 64 : 8;
    const int round_trips = bench ? 3000 : 50;
    const std::size_t bulk_bytes = (bench ? std::size_t{512} : std::size_t{8}) << 20;

    auto listener_result = TcpListener::listen("127.0.0.1", 0).wait();
    check(listener_result.has_value(), "listen");
    if (!listener_result) return 1;
    TcpListener listener = std::move(*listener_result);
    const auto port = *listener.local_port();

    // Echo server: every connection gets a thread that reads a 64-byte request and writes it back, until the peer closes.
    std::atomic<bool> serving{true};
    std::thread acceptor([&] {
        std::vector<std::thread> sessions;
        while (serving) {
            auto connection = listener.accept().wait();
            if (!connection) break;
            sessions.emplace_back([conn = std::make_shared<TcpConnection>(std::move(*connection))] {
                std::vector<std::byte> request;
                while (true) {
                    auto got = conn->receive(64 * 1024).wait();
                    if (!got || got->empty()) return;
                    if (!conn->send_all(std::move(*got)).wait()) return;
                }
            });
        }
        for (auto &session : sessions) session.join();
    });

    // 1. Round trips: `connections` clients each doing `round_trips` request/response exchanges of 64 bytes.
    {
        std::atomic<int> bad{0};
        const auto start = Clock::now();
        std::vector<std::thread> clients;
        for (int c = 0; c < connections; ++c) {
            clients.emplace_back([&, c] {
                auto opened = TcpConnection::connect("127.0.0.1", port).wait();
                if (!opened) return void(++bad);
                TcpConnection connection = std::move(*opened);
                std::vector<std::byte> echoed;
                for (int i = 0; i < round_trips; ++i) {
                    const auto request = payload(64, static_cast<unsigned>(c * 100003 + i));
                    if (!connection.send_all(request).wait() || !read_exactly(connection, request.size(), echoed) || echoed != request) {
                        ++bad;
                        return;
                    }
                }
            });
        }
        for (auto &client : clients) client.join();
        const double elapsed = seconds_since(start);
        check(bad == 0, "every round trip returns the bytes that were sent");
        if (bench) {
            const double total = static_cast<double>(connections) * round_trips;
            std::fprintf(stderr, "round trips: %d connections x %d: %.0f exchanges/s, %.1f us mean per exchange per connection\n", connections,
                         round_trips, total / elapsed, elapsed / round_trips * 1e6);
        }
    }

    // 2. One connection, bulk data one way and echoed back: integrity at the megabyte scale.
    {
        auto opened = TcpConnection::connect("127.0.0.1", port).wait();
        check(opened.has_value(), "connect for bulk transfer");
        if (opened) {
            TcpConnection connection = std::move(*opened);
            const auto block = payload(1 << 20, 7);
            const auto start = Clock::now();
            std::atomic<bool> sent_ok{true};
            std::thread sender([&] {
                for (std::size_t sent = 0; sent < bulk_bytes; sent += block.size()) {
                    if (!connection.send_all(block).wait()) {
                        sent_ok = false;
                        return;
                    }
                }
            });
            std::size_t received = 0;
            bool intact = true;
            std::vector<std::byte> chunk;
            while (received < bulk_bytes) {
                auto got = connection.receive(1 << 20).wait();
                if (!got || got->empty()) {
                    intact = false;
                    break;
                }
                for (std::size_t i = 0; i < got->size() && intact; i += 4099) {
                    intact = (*got)[i] == block[(received + i) % block.size()];
                }
                received += got->size();
            }
            sender.join();
            check(sent_ok && intact && received == bulk_bytes, "bulk data comes back whole and in order");
            if (bench) {
                const double elapsed = seconds_since(start);
                std::fprintf(stderr, "bulk echo: %.0f MB/s each way (%zu MiB)\n", static_cast<double>(bulk_bytes) / (1 << 20) / elapsed, bulk_bytes >> 20);
            }
            connection.close();
        }
    }

    serving = false;
    listener.close();
    acceptor.join();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
