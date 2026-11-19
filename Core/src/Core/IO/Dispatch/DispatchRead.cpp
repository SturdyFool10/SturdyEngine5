#include <Core/IO/Dispatch/DispatchRead.hpp>

#if defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>

#include <tracy/Tracy.hpp>

namespace SFT::Core {

    namespace {
        constexpr std::size_t kChunkBytes = 2u << 20;
    } // namespace

    std::optional<std::vector<std::byte>> read_file_dispatch(const std::filesystem::path &path) {
        ZoneScopedN("Core::read_file_dispatch");

        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return std::nullopt;
        }
        struct stat info {};
        if (::fstat(fd, &info) != 0 || info.st_size <= 0) {
            ::close(fd);
            return std::nullopt;
        }
        const std::size_t file_size = static_cast<std::size_t>(info.st_size);
        ::fcntl(fd, F_RDAHEAD, 1);

        std::vector<std::byte> bytes(file_size);
        std::byte *const base = bytes.data();
        const std::size_t chunk_count = (file_size + kChunkBytes - 1) / kChunkBytes;
        std::atomic<bool> failed{false};
        std::atomic<bool> *const failed_flag = &failed; // a block copies what it captures; the atomic itself cannot be copied

        // dispatch_apply runs the iterations concurrently on the global queue and returns when all are done, the calling thread
        // included in the work.
        dispatch_apply(chunk_count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(std::size_t chunk) {
            if (failed_flag->load(std::memory_order_relaxed)) {
                return;
            }
            const std::size_t begin = chunk * kChunkBytes;
            const std::size_t end = std::min(file_size, begin + kChunkBytes);
            std::size_t done = begin;
            while (done < end) {
                const ssize_t got = ::pread(fd, base + done, end - done, static_cast<off_t>(done));
                if (got < 0 && errno == EINTR) {
                    continue;
                }
                if (got <= 0) {
                    failed_flag->store(true, std::memory_order_relaxed);
                    return;
                }
                done += static_cast<std::size_t>(got);
            }
        });

        ::close(fd);
        if (failed.load()) {
            return std::nullopt;
        }
        return bytes;
    }

} // namespace SFT::Core

#endif // defined(__APPLE__)
