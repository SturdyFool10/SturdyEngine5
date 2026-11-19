#include <Core/IO/IoUring/IoUringBackend.hpp>


#if defined(__linux__)

#include <Foundation/Foundation.hpp>

#include <linux/io_uring.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <tracy/Tracy.hpp>


#if !defined(SYS_io_uring_setup)
    #define SYS_io_uring_setup 425
#endif
#if !defined(SYS_io_uring_enter)
    #define SYS_io_uring_enter 426
#endif

namespace SFT::Core {

    namespace {

        [[nodiscard]] int sys_io_uring_setup(unsigned entries, io_uring_params *params) {
            return static_cast<int>(syscall(SYS_io_uring_setup, entries, params));
        }

        [[nodiscard]] int sys_io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags) {
            return static_cast<int>(syscall(SYS_io_uring_enter, ring_fd, to_submit, min_complete, flags, nullptr, 0));
        }

        /// Reads go out as this many bytes per request, `kQueueDepth` of them in flight at once: that is what keeps an NVMe
        /// drive's queues full (one huge request is one queue slot) and lets the kernel copy cached pages on several cores.
        constexpr usize kChunkBytes = 1u << 20;
        constexpr unsigned kQueueDepth = 16;
        constexpr unsigned kRingEntries = 32;

        /// One ring per calling thread. Loads come from many threads at once (asset decode workers, the streamer, the IO
        /// pool), and a shared ring behind a mutex would serialize them -- the opposite of what a fast path is for. A ring costs
        /// three small mappings, created on a thread's first accelerated read and released when the thread exits.
        struct Ring {
            int ring_fd = -1;
            void *sq_ring_ptr = nullptr;
            usize sq_ring_size = 0;
            void *cq_ring_ptr = nullptr;
            usize cq_ring_size = 0;
            io_uring_sqe *sqes = nullptr;
            usize sqes_size = 0;

            unsigned *sq_head = nullptr;
            unsigned *sq_tail = nullptr;
            unsigned *sq_ring_mask = nullptr;
            unsigned *sq_array = nullptr;

            unsigned *cq_head = nullptr;
            unsigned *cq_tail = nullptr;
            unsigned *cq_ring_mask = nullptr;
            io_uring_cqe *cqes = nullptr;

            bool attempted = false;
            bool ready = false;

            Ring() = default;
            Ring(const Ring &) = delete;
            Ring &operator=(const Ring &) = delete;
            ~Ring() { teardown(); }

            void teardown() noexcept {
                if (sqes != nullptr) {
                    munmap(sqes, sqes_size);
                    sqes = nullptr;
                }
                if (cq_ring_ptr != nullptr && cq_ring_ptr != sq_ring_ptr) {
                    munmap(cq_ring_ptr, cq_ring_size);
                }
                cq_ring_ptr = nullptr;
                if (sq_ring_ptr != nullptr) {
                    munmap(sq_ring_ptr, sq_ring_size);
                    sq_ring_ptr = nullptr;
                }
                if (ring_fd >= 0) {
                    close(ring_fd);
                    ring_fd = -1;
                }
                ready = false;
            }
        };

        /// -1 until a ring has been tried, then 1 (rings work) or 0 (the kernel refuses: old kernel, seccomp, container
        /// policy). One refusal turns the path off for every thread and is logged once, not once per thread.
        std::atomic<int> g_ring_support{-1};

        /// Sets the ring up; false (with the ring torn down) when anything fails.
        [[nodiscard]] bool setup_ring(Ring &r) noexcept {
            io_uring_params params{};
            r.ring_fd = sys_io_uring_setup(kRingEntries, &params);
            if (r.ring_fd < 0) {
                const int error = errno;
                int expected = -1;
                if (g_ring_support.compare_exchange_strong(expected, 0)) {
                    Foundation::log_warn("io_uring: io_uring_setup failed (errno={}); using standard file I/O.", error);
                }
                return false;
            }

            r.sq_ring_size = params.sq_off.array + static_cast<usize>(params.sq_entries) * sizeof(unsigned);
            r.cq_ring_size = params.cq_off.cqes + static_cast<usize>(params.cq_entries) * sizeof(io_uring_cqe);
            const bool single_mmap = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
            if (single_mmap) {
                r.sq_ring_size = r.cq_ring_size = std::max(r.sq_ring_size, r.cq_ring_size);
            }

            r.sq_ring_ptr = mmap(nullptr, r.sq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r.ring_fd, IORING_OFF_SQ_RING);
            if (r.sq_ring_ptr == MAP_FAILED) {
                r.sq_ring_ptr = nullptr;
                r.teardown();
                return false;
            }
            if (single_mmap) {
                r.cq_ring_ptr = r.sq_ring_ptr;
            } else {
                r.cq_ring_ptr = mmap(nullptr, r.cq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r.ring_fd, IORING_OFF_CQ_RING);
                if (r.cq_ring_ptr == MAP_FAILED) {
                    r.cq_ring_ptr = nullptr;
                    r.teardown();
                    return false;
                }
            }
            r.sqes_size = static_cast<usize>(params.sq_entries) * sizeof(io_uring_sqe);
            r.sqes = static_cast<io_uring_sqe *>(mmap(nullptr, r.sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r.ring_fd, IORING_OFF_SQES));
            if (r.sqes == MAP_FAILED) {
                r.sqes = nullptr;
                r.teardown();
                return false;
            }

            auto *sq_base = static_cast<std::byte *>(r.sq_ring_ptr);
            r.sq_head = reinterpret_cast<unsigned *>(sq_base + params.sq_off.head);
            r.sq_tail = reinterpret_cast<unsigned *>(sq_base + params.sq_off.tail);
            r.sq_ring_mask = reinterpret_cast<unsigned *>(sq_base + params.sq_off.ring_mask);
            r.sq_array = reinterpret_cast<unsigned *>(sq_base + params.sq_off.array);

            auto *cq_base = static_cast<std::byte *>(r.cq_ring_ptr);
            r.cq_head = reinterpret_cast<unsigned *>(cq_base + params.cq_off.head);
            r.cq_tail = reinterpret_cast<unsigned *>(cq_base + params.cq_off.tail);
            r.cq_ring_mask = reinterpret_cast<unsigned *>(cq_base + params.cq_off.ring_mask);
            r.cqes = reinterpret_cast<io_uring_cqe *>(cq_base + params.cq_off.cqes);

            r.ready = true;
            int expected = -1;
            g_ring_support.compare_exchange_strong(expected, 1);
            return true;
        }

        /// The calling thread's ring, or nullptr when io_uring is not usable here.
        [[nodiscard]] Ring *thread_ring() noexcept {
            thread_local Ring ring;
            if (!ring.attempted) {
                if (g_ring_support.load(std::memory_order_acquire) == 0) {
                    return nullptr;
                }
                ring.attempted = true;
                (void)setup_ring(ring);
            }
            return ring.ready ? &ring : nullptr;
        }

        /// One chunk of the file in flight (or waiting to be retried after a short read).
        struct Slot {
            u64 offset = 0;
            usize length = 0;
        };

        void queue_read(Ring &r, int fd, std::byte *base, const Slot &slot, unsigned slot_index) noexcept {
            std::atomic_ref<unsigned> tail_ref(*r.sq_tail);
            const unsigned tail = tail_ref.load(std::memory_order_relaxed);
            const unsigned index = tail & *r.sq_ring_mask;
            io_uring_sqe &sqe = r.sqes[index];
            std::memset(&sqe, 0, sizeof(sqe));
            sqe.opcode = IORING_OP_READ;
            sqe.fd = fd;
            sqe.off = slot.offset;
            sqe.addr = static_cast<__u64>(reinterpret_cast<std::uintptr_t>(base + slot.offset));
            sqe.len = static_cast<unsigned>(slot.length);
            sqe.user_data = slot_index;
            r.sq_array[index] = index;
            tail_ref.store(tail + 1, std::memory_order_release);
        }

    } // namespace

    bool io_uring_available() noexcept {
        return thread_ring() != nullptr;
    }

    RHI::RhiExpected<std::vector<std::byte>> read_file_io_uring(const std::filesystem::path &path) {
        ZoneScopedN("Core::read_file_io_uring");

        Ring *ring = thread_ring();
        if (ring == nullptr) {
            return unexpected(
                RHI::rhi_error(RHI::RhiErrorCode::Unsupported, "read_file_io_uring: io_uring is unavailable on this system."));
        }
        Ring &r = *ring;

        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return unexpected(
                RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, "read_file_io_uring: could not open '" + path.string() + "'."));
        }
        struct stat file_stat {};
        if (fstat(fd, &file_stat) != 0 || file_stat.st_size <= 0) {
            close(fd);
            return unexpected(
                RHI::rhi_error(RHI::RhiErrorCode::OperationFailed, "read_file_io_uring: could not stat '" + path.string() + "'."));
        }
        const u64 file_size = static_cast<u64>(file_stat.st_size);
        posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

        std::vector<std::byte> bytes(static_cast<usize>(file_size));
        std::byte *const base = bytes.data();

        // Chunks go out `kQueueDepth` at a time; a chunk that comes back short (a signal, the kernel's per-request cap) is
        // resubmitted for the rest. There is no per-file size limit: the 4 GiB cap of a single request does not apply.
        Slot slots[kQueueDepth];
        unsigned free_slots[kQueueDepth];
        unsigned free_count = kQueueDepth;
        for (unsigned i = 0; i < kQueueDepth; ++i) {
            free_slots[i] = i;
        }
        u64 next_offset = 0;
        unsigned inflight = 0;
        bool failed = false;

        std::atomic_ref<unsigned> cq_tail_ref(*r.cq_tail);
        std::atomic_ref<unsigned> cq_head_ref(*r.cq_head);

        while (inflight > 0 || (!failed && next_offset < file_size)) {
            while (!failed && free_count > 0 && next_offset < file_size) {
                const unsigned slot_index = free_slots[--free_count];
                Slot &slot = slots[slot_index];
                slot.offset = next_offset;
                slot.length = static_cast<usize>(std::min<u64>(kChunkBytes, file_size - next_offset));
                next_offset += slot.length;
                queue_read(r, fd, base, slot, slot_index);
                ++inflight;
            }

            // Everything the kernel has not consumed yet, counted from the ring itself: an enter interrupted by a signal may have
            // consumed some entries and not others, and a stale count would strand the rest.
            const unsigned pending = std::atomic_ref<unsigned>(*r.sq_tail).load(std::memory_order_relaxed) -
                                     std::atomic_ref<unsigned>(*r.sq_head).load(std::memory_order_acquire);
            const int entered = sys_io_uring_enter(r.ring_fd, pending, 1, IORING_ENTER_GETEVENTS);
            if (entered < 0 && errno != EINTR && errno != EAGAIN && errno != EBUSY) {
                // The ring is unusable. Requests already queued may still complete into `bytes`, so give up on this thread's
                // ring for good rather than return a buffer the kernel might still write to.
                r.teardown();
                close(fd);
                return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                                  "read_file_io_uring: io_uring_enter failed for '" + path.string() + "'."));
            }

            unsigned head = cq_head_ref.load(std::memory_order_relaxed);
            const unsigned tail = cq_tail_ref.load(std::memory_order_acquire);
            while (head != tail) {
                const io_uring_cqe cqe = r.cqes[head & *r.cq_ring_mask];
                ++head;
                --inflight;
                const unsigned slot_index = static_cast<unsigned>(cqe.user_data);
                Slot &slot = slots[slot_index];
                if (cqe.res < 0) {
                    if (cqe.res == -EINTR || cqe.res == -EAGAIN) {
                        queue_read(r, fd, base, slot, slot_index);
                        ++inflight; // submitted by the next enter, which counts what the ring still holds
                        continue;
                    }
                    failed = true;
                } else if (cqe.res == 0) {
                    failed = true; // the file is shorter than it was a moment ago
                } else if (static_cast<usize>(cqe.res) < slot.length) {
                    slot.offset += static_cast<u64>(cqe.res);
                    slot.length -= static_cast<usize>(cqe.res);
                    queue_read(r, fd, base, slot, slot_index);
                    ++inflight;
                    continue;
                }
                free_slots[free_count++] = slot_index;
            }
            cq_head_ref.store(head, std::memory_order_release);
        }

        close(fd);
        if (failed) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_io_uring: short or failed read for '" + path.string() + "'."));
        }
        return bytes;
    }

} // namespace SFT::Core

#endif // defined(__linux__)
