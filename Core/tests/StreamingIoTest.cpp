/// The platform fast path for whole-file reads (io_uring / DirectStorage / overlapped Win32 / GCD): the bytes must be exactly
/// the file's, for sizes that are not a multiple of any chunk, and many threads must be able to read at once. On a machine that
/// refuses the fast path (a container without io_uring) `read_file_accelerated` returns nullopt and the test reports that rather
/// than failing: the portable path is `Foundation::Io::read_file`'s job, checked here too.

#include <Core/StreamingIo.hpp>

#include <Foundation/FileIo.hpp>
#include <Foundation/Foundation.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

    using namespace SFT;

    std::vector<std::byte> pattern(std::size_t size, std::uint32_t seed) {
        std::vector<std::byte> bytes(size);
        std::uint32_t state = seed * 2654435761u + 1u;
        for (std::byte &b : bytes) {
            state = state * 1664525u + 1013904223u;
            b = static_cast<std::byte>(state >> 24);
        }
        return bytes;
    }

} // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "sturdy_streaming_io_test";
    std::filesystem::create_directories(directory);

    struct Case {
        std::string name;
        std::vector<std::byte> bytes;
    };
    std::vector<Case> cases;
    // Just above the accelerator threshold, odd sizes around the 1 MiB chunk, and one spanning many chunks and queue refills.
    for (const std::size_t size : {std::size_t{300} * 1024 + 7, std::size_t{1} << 20, (std::size_t{5} << 20) + 37, (std::size_t{40} << 20) + 12345}) {
        cases.push_back({"file_" + std::to_string(size) + ".bin", pattern(size, static_cast<std::uint32_t>(size))});
        std::ofstream out(directory / cases.back().name, std::ios::binary);
        out.write(reinterpret_cast<const char *>(cases.back().bytes.data()), static_cast<std::streamsize>(cases.back().bytes.size()));
    }

    int failures = 0;
    bool accelerated_any = false;
    for (const Case &c : cases) {
        const auto path = directory / c.name;
        if (auto fast = Core::read_file_accelerated(path)) {
            accelerated_any = true;
            if (*fast != c.bytes) {
                Foundation::log_error("accelerated read of {} returned different bytes", c.name);
                ++failures;
            }
        }
        // The engine-wide entry point must agree whichever route it took.
        Foundation::Io::set_file_accelerator([](const std::filesystem::path &p) { return Core::read_file_accelerated(p); });
        auto routed = Foundation::Io::read_file(path);
        if (!routed || *routed != c.bytes) {
            Foundation::log_error("Io::read_file of {} did not return the file's bytes", c.name);
            ++failures;
        }
    }
    Foundation::log_info("accelerated path {}", accelerated_any ? "active" : "unavailable here (portable reads used)");

    // Many threads, each reading every file several times, all at once: no shared state may corrupt or serialize them into errors.
    std::atomic<int> thread_failures{0};
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back([&] {
                for (int round = 0; round < 3; ++round) {
                    for (const Case &c : cases) {
                        auto routed = Foundation::Io::read_file(directory / c.name);
                        if (!routed || *routed != c.bytes) {
                            ++thread_failures;
                        }
                    }
                }
            });
        }
        for (std::thread &thread : threads) {
            thread.join();
        }
    }
    if (thread_failures.load() != 0) {
        Foundation::log_error("{} concurrent reads returned wrong data", thread_failures.load());
        ++failures;
    }

    std::filesystem::remove_all(directory);

    // Optional throughput comparison (STURDY_IO_BENCH_DIR=<a directory on the disk to measure; not tmpfs>): the same large file read
    // cold by one thread of plain pread, by the portable route, and by the platform fast path. Prints, never fails.
    if (const char *bench_dir = std::getenv("STURDY_IO_BENCH_DIR")) {
        const std::filesystem::path path = std::filesystem::path(bench_dir) / "sturdy_io_bench.bin";
        const std::size_t megabytes = 1024;
        {
            const auto chunk = pattern(8u << 20, 5);
            std::ofstream out(path, std::ios::binary);
            for (std::size_t i = 0; i < megabytes / 8; ++i) {
                out.write(reinterpret_cast<const char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            }
        }
        const auto drop_cache = [&] {
            const int fd = ::open(path.c_str(), O_RDONLY);
            if (fd >= 0) {
                ::fdatasync(fd);
                ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
                ::close(fd);
            }
        };
        const auto time_it = [&](const char *name, auto &&read) {
            double best = 1e30;
            for (int run = 0; run < 3; ++run) {
                drop_cache();
                const auto start = std::chrono::steady_clock::now();
                const bool ok = read();
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                best = ok ? std::min(best, seconds) : best;
            }
            // Written to stderr directly: info-level log lines are trace-only, and this report is the point of the mode.
            std::fprintf(stderr, "%-34s %8.0f MB/s (best of 3, cold cache, %zu MiB)\n", name, static_cast<double>(megabytes) / best, megabytes);
        };
        time_it("one thread, plain pread", [&] {
            const int fd = ::open(path.c_str(), O_RDONLY);
            std::vector<std::byte> bytes(megabytes << 20);
            std::size_t done = 0;
            while (done < bytes.size()) {
                const ssize_t got = ::pread(fd, bytes.data() + done, std::min<std::size_t>(bytes.size() - done, 1u << 30), static_cast<off_t>(done));
                if (got <= 0) break;
                done += static_cast<std::size_t>(got);
            }
            ::close(fd);
            return done == bytes.size();
        });
        Foundation::Io::set_file_accelerator({});
        time_it("Io::read_file, portable", [&] { return Foundation::Io::read_file(path).has_value(); });
        time_it("Core accelerated (io_uring)", [&] { return Core::read_file_accelerated(path).has_value(); });
        std::filesystem::remove(path);
    }

    return failures == 0 ? 0 : 1;
}
