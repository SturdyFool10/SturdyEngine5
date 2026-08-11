#include "IoUringBackend.hpp"

// Talks to io_uring directly through its raw syscalls (io_uring_setup/io_uring_enter) and the
// kernel-shared submission/completion ring memory (mmap()ed from the ring fd), rather than linking
// liburing. liburing has no portable CMake package (its own build is a hand-rolled ./configure +
// Make, awkward to FetchContent alongside this engine's other CMake-native dependencies), and the
// io_uring UAPI itself (<linux/io_uring.h>, stable since Linux 5.1) is small enough for the single
// synchronous "read one whole file" operation this backend needs that going straight to the
// syscalls is less code and one fewer vendored dependency than wrapping liburing would be. This is
// the same shape of tradeoff DirectStorageBackend.cpp makes on Windows, just via COM there instead
// of raw syscalls.
//
// Everything below is Linux-only and guarded to match IoUringBackend.hpp's own guard: Core's
// CMakeLists never adds this file to a non-Linux build, but nothing stops a reader-side tool from
// parsing it with another platform's flags (clangd infers a command from a sibling TU for any file
// missing a compile_commands.json entry, so a Windows view lands here with a Windows target). The
// guard keeps the Linux UAPI headers and syscalls out of any such parse instead of relying on the
// build system's per-OS file list being the only thing that ever looks at this file.
#if defined(__linux__)

#include <Foundation/src/Foundation.hpp>

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

// glibc only gained wrapper functions for these in 2.36 (mid-2022) -- go straight to syscall(2)
// with the raw numbers so this also builds against older glibc, matching liburing's own approach.
// Values are the stable x86-64 syscall table entries (SFT is x86-64-only today, see
// STURDY_GRAPHICS_PLATFORM_ARCH_X86_64 in cmake/SturdyPlatform.cmake) -- revisit if this engine
// ever grows another Linux target architecture.
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

        // One persistent ring (8 SQEs -- more would buy nothing, see the header's own doc comment
        // for why this backend only ever has one request in flight), reused under state_mutex() for
        // every read_file_io_uring() call. Mirrors DirectStorageBackend's single persistent-queue
        // design on Windows.
        struct IoUringState {
            int ring_fd = -1;

            void *sq_ring_ptr = nullptr;
            usize sq_ring_size = 0;
            void *cq_ring_ptr = nullptr; // == sq_ring_ptr when IORING_FEAT_SINGLE_MMAP.
            usize cq_ring_size = 0;
            io_uring_sqe *sqes = nullptr;
            usize sqes_size = 0;

            unsigned *sq_tail = nullptr;
            unsigned *sq_ring_mask = nullptr;
            unsigned *sq_array = nullptr;

            unsigned *cq_head = nullptr;
            unsigned *cq_tail = nullptr;
            unsigned *cq_ring_mask = nullptr;
            io_uring_cqe *cqes = nullptr;

            bool init_attempted = false;
            bool available = false;
        };

        IoUringState &state() {
            static IoUringState instance;
            return instance;
        }

        std::mutex &state_mutex() {
            static std::mutex instance;
            return instance;
        }

        void teardown_partial_locked(IoUringState &s) {
            if (s.sqes != nullptr) {
                munmap(s.sqes, s.sqes_size);
                s.sqes = nullptr;
            }
            if (s.cq_ring_ptr != nullptr && s.cq_ring_ptr != s.sq_ring_ptr) {
                munmap(s.cq_ring_ptr, s.cq_ring_size);
            }
            s.cq_ring_ptr = nullptr;
            if (s.sq_ring_ptr != nullptr) {
                munmap(s.sq_ring_ptr, s.sq_ring_size);
                s.sq_ring_ptr = nullptr;
            }
            if (s.ring_fd >= 0) {
                close(s.ring_fd);
                s.ring_fd = -1;
            }
        }

        // Callers must hold state_mutex().
        [[nodiscard]] bool ensure_initialized_locked() {
            IoUringState &s = state();
            if (s.init_attempted) {
                return s.available;
            }
            s.init_attempted = true;

            io_uring_params params{};
            s.ring_fd = sys_io_uring_setup(8, &params);
            if (s.ring_fd < 0) {
                Foundation::log_warn("io_uring: io_uring_setup failed (errno={}); falling back to standard file I/O.", errno);
                return false;
            }

            s.sq_ring_size = params.sq_off.array + static_cast<usize>(params.sq_entries) * sizeof(unsigned);
            s.cq_ring_size = params.cq_off.cqes + static_cast<usize>(params.cq_entries) * sizeof(io_uring_cqe);
            const bool single_mmap = (params.features & IORING_FEAT_SINGLE_MMAP) != 0;
            if (single_mmap) {
                s.sq_ring_size = s.cq_ring_size = std::max(s.sq_ring_size, s.cq_ring_size);
            }

            s.sq_ring_ptr = mmap(nullptr, s.sq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s.ring_fd,
                                  IORING_OFF_SQ_RING);
            if (s.sq_ring_ptr == MAP_FAILED) {
                Foundation::log_warn("io_uring: mmap(SQ ring) failed; falling back to standard file I/O.");
                s.sq_ring_ptr = nullptr;
                teardown_partial_locked(s);
                return false;
            }

            if (single_mmap) {
                s.cq_ring_ptr = s.sq_ring_ptr;
            } else {
                s.cq_ring_ptr = mmap(nullptr, s.cq_ring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s.ring_fd,
                                      IORING_OFF_CQ_RING);
                if (s.cq_ring_ptr == MAP_FAILED) {
                    Foundation::log_warn("io_uring: mmap(CQ ring) failed; falling back to standard file I/O.");
                    s.cq_ring_ptr = nullptr;
                    teardown_partial_locked(s);
                    return false;
                }
            }

            s.sqes_size = static_cast<usize>(params.sq_entries) * sizeof(io_uring_sqe);
            s.sqes = static_cast<io_uring_sqe *>(
                mmap(nullptr, s.sqes_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, s.ring_fd, IORING_OFF_SQES));
            if (s.sqes == MAP_FAILED) {
                Foundation::log_warn("io_uring: mmap(SQEs) failed; falling back to standard file I/O.");
                s.sqes = nullptr;
                teardown_partial_locked(s);
                return false;
            }

            auto *sq_base = static_cast<std::byte *>(s.sq_ring_ptr);
            s.sq_tail = reinterpret_cast<unsigned *>(sq_base + params.sq_off.tail);
            s.sq_ring_mask = reinterpret_cast<unsigned *>(sq_base + params.sq_off.ring_mask);
            s.sq_array = reinterpret_cast<unsigned *>(sq_base + params.sq_off.array);

            auto *cq_base = static_cast<std::byte *>(s.cq_ring_ptr);
            s.cq_head = reinterpret_cast<unsigned *>(cq_base + params.cq_off.head);
            s.cq_tail = reinterpret_cast<unsigned *>(cq_base + params.cq_off.tail);
            s.cq_ring_mask = reinterpret_cast<unsigned *>(cq_base + params.cq_off.ring_mask);
            s.cqes = reinterpret_cast<io_uring_cqe *>(cq_base + params.cq_off.cqes);

            s.available = true;
            return true;
        }

    } // namespace

    bool io_uring_available() noexcept {
        std::lock_guard<std::mutex> lock(state_mutex());
        return ensure_initialized_locked();
    }

    RHI::RhiExpected<std::vector<std::byte>> read_file_io_uring(const std::filesystem::path &path) {
        ZoneScopedN("Core::read_file_io_uring");

        std::lock_guard<std::mutex> lock(state_mutex());
        if (!ensure_initialized_locked()) {
            return unexpected(
                RHI::rhi_error(RHI::RhiErrorCode::Unsupported, "read_file_io_uring: io_uring is unavailable on this system."));
        }
        IoUringState &s = state();

        const int fd = open(path.c_str(), O_RDONLY);
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
        // IORING_OP_READ's len field is a plain __u32 -- same 4GiB-per-request ceiling
        // DirectStorageBackend imposes on Windows (see its own doc comment); no texture asset this
        // pipeline handles comes close, so this is a hard reject rather than a chunking loop.
        if (static_cast<u64>(file_stat.st_size) > std::numeric_limits<unsigned>::max()) {
            close(fd);
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_io_uring: '" + path.string() + "' exceeds the 4GiB per-request limit."));
        }
        const usize file_size = static_cast<usize>(file_stat.st_size);

        std::vector<std::byte> bytes(file_size);

        // Only one request is ever in flight on this ring (see the header's own doc comment), so
        // the submission slot at the current tail is always free -- no need to check against
        // sq_head/ring capacity first.
        std::atomic_ref<unsigned> sq_tail_ref(*s.sq_tail);
        const unsigned tail = sq_tail_ref.load(std::memory_order_relaxed);
        const unsigned index = tail & *s.sq_ring_mask;

        io_uring_sqe &sqe = s.sqes[index];
        std::memset(&sqe, 0, sizeof(sqe));
        sqe.opcode = IORING_OP_READ;
        sqe.fd = fd;
        sqe.off = 0;
        sqe.addr = static_cast<__u64>(reinterpret_cast<std::uintptr_t>(bytes.data()));
        sqe.len = static_cast<unsigned>(file_size);
        sqe.user_data = 1;

        s.sq_array[index] = index;
        // Release: makes the SQE contents and the array write above visible to the kernel before
        // it observes the new tail value.
        sq_tail_ref.store(tail + 1, std::memory_order_release);

        const int enter_result = sys_io_uring_enter(s.ring_fd, 1, 1, IORING_ENTER_GETEVENTS);
        if (enter_result < 0) {
            close(fd);
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_io_uring: io_uring_enter failed for '" + path.string() + "'."));
        }

        // io_uring_enter(..., min_complete=1, IORING_ENTER_GETEVENTS) blocks until at least our one
        // completion is posted, so the CQE at the current head is guaranteed ready -- the acquire
        // load here is what makes that CQE's contents (written by the kernel before it advanced
        // cq_tail) visible to this thread.
        std::atomic_ref<unsigned> cq_tail_ref(*s.cq_tail);
        std::atomic_ref<unsigned> cq_head_ref(*s.cq_head);
        (void)cq_tail_ref.load(std::memory_order_acquire);
        const unsigned head = cq_head_ref.load(std::memory_order_relaxed);
        const io_uring_cqe cqe = s.cqes[head & *s.cq_ring_mask];
        cq_head_ref.store(head + 1, std::memory_order_release);

        close(fd);

        if (cqe.res < 0 || static_cast<usize>(cqe.res) != file_size) {
            return unexpected(RHI::rhi_error(RHI::RhiErrorCode::OperationFailed,
                                              "read_file_io_uring: short or failed read for '" + path.string() + "'."));
        }

        return bytes;
    }

} // namespace SFT::Core

#endif // defined(__linux__)
