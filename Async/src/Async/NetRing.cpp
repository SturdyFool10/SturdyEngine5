#include <Async/NetRing.hpp>

#include <cerrno>

#if defined(STURDY_ASYNC_IO_URING) && defined(__linux__)

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Design: one thread owns the ring. Every other thread only appends a request to a queue; the ring thread turns the queue into
// submissions (a whole batch per io_uring_enter), waits, and runs the completions. That keeps the kernel's completion work on one
// thread (no interrupting whichever thread happened to submit), lets the ring use single-issuer / deferred-taskrun mode where the
// kernel has it, needs no lock around the submission queue, and batches syscalls under load. The cost is one hand-off per request,
// which is why the ring is worth it for many busy sockets and not for one chatty one -- see the benchmark in NetBenchTest.

namespace SFT::Async::Detail::netring {

    namespace {

        struct Op {
            Completion done;
        };

        struct Request {
            enum class Kind { Recv, Send, RecvMsg, SendMsg, Accept, Connect, Cancel };
            Kind kind = Kind::Recv;
            int fd = -1;
            void *buffer = nullptr;
            std::size_t size = 0;
            msghdr *message = nullptr;
            sockaddr *address = nullptr;
            unsigned *address_length = nullptr;
            unsigned connect_length = 0;
            Completion done;
        };

        /// Cancel-by-descriptor (IORING_ASYNC_CANCEL_FD) arrived in Linux 5.19; the other opcodes used are older.
        [[nodiscard]] bool kernel_new_enough() noexcept {
            utsname info{};
            if (::uname(&info) != 0) return false;
            int major = 0;
            int minor = 0;
            if (std::sscanf(info.release, "%d.%d", &major, &minor) != 2) return false;
            return major > 5 || (major == 5 && minor >= 19);
        }

        class Ring {
          public:
            Ring() {
                if (std::getenv("STURDY_ASYNC_NO_IO_URING") != nullptr || !kernel_new_enough()) return;
                wake_fd_ = ::eventfd(0, EFD_CLOEXEC);
                if (wake_fd_ < 0) return;
                std::thread([this] { run(); }).detach(); // lives for the process (see ring())
                std::unique_lock lock(init_mutex_);
                init_done_.wait(lock, [this] { return init_finished_; });
            }

            [[nodiscard]] bool ready() const noexcept { return ready_; }

            void enqueue(Request request) {
                {
                    std::lock_guard lock(queue_mutex_);
                    queue_.push_back(std::move(request));
                    queued_.fetch_add(1, std::memory_order_release);
                }
                if (sleeping_.load(std::memory_order_seq_cst)) {
                    const std::uint64_t one = 1;
                    (void)!::write(wake_fd_, &one, sizeof one);
                }
            }

          private:
            static constexpr std::uint64_t kWakeTag = 1; ///< user_data of the wake-up read (real operations carry an Op pointer)

            void finish_init(bool ok) {
                {
                    std::lock_guard lock(init_mutex_);
                    ready_ = ok;
                    init_finished_ = true;
                }
                init_done_.notify_all();
            }

            /// The most modern setup flags the kernel accepts. Single-issuer + deferred task-run (6.1) makes the kernel finish network
            /// completions only when this thread asks, instead of interrupting threads; each older level degrades gracefully.
            [[nodiscard]] bool init_ring() {
                const unsigned attempts[] = {
                    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN,
                    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN,
                    IORING_SETUP_COOP_TASKRUN,
                    0u,
                };
                for (const unsigned flags : attempts) {
                    io_uring_params params{};
                    params.flags = flags;
                    if (io_uring_queue_init_params(4096, &ring_, &params) == 0) return true;
                }
                return false;
            }

            [[nodiscard]] bool opcodes_supported() {
                io_uring_probe *probe = io_uring_get_probe_ring(&ring_);
                if (probe == nullptr) return true; // cannot tell; the first use will fail loudly rather than silently
                const int needed[] = {IORING_OP_RECV, IORING_OP_SEND, IORING_OP_RECVMSG, IORING_OP_SENDMSG,
                                      IORING_OP_ACCEPT, IORING_OP_CONNECT, IORING_OP_ASYNC_CANCEL, IORING_OP_READ};
                bool all = true;
                for (const int opcode : needed) all = all && io_uring_opcode_supported(probe, opcode) != 0;
                io_uring_free_probe(probe);
                return all;
            }

            io_uring_sqe *next_sqe() {
                io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
                if (sqe == nullptr) {
                    io_uring_submit(&ring_); // submission queue full: hand what is queued to the kernel and try again
                    sqe = io_uring_get_sqe(&ring_);
                }
                return sqe;
            }

            void arm_wake() {
                if (io_uring_sqe *sqe = next_sqe()) {
                    io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof wake_value_, 0);
                    io_uring_sqe_set_data64(sqe, kWakeTag);
                }
            }

            void prepare(Request &r) {
                io_uring_sqe *sqe = next_sqe();
                if (sqe == nullptr) {
                    if (r.done) r.done(-EBUSY);
                    return;
                }
                switch (r.kind) {
                    case Request::Kind::Recv: io_uring_prep_recv(sqe, r.fd, r.buffer, r.size, 0); break;
                    case Request::Kind::Send: io_uring_prep_send(sqe, r.fd, r.buffer, r.size, MSG_NOSIGNAL); break;
                    case Request::Kind::RecvMsg: io_uring_prep_recvmsg(sqe, r.fd, r.message, 0); break;
                    case Request::Kind::SendMsg: io_uring_prep_sendmsg(sqe, r.fd, r.message, MSG_NOSIGNAL); break;
                    case Request::Kind::Accept: io_uring_prep_accept(sqe, r.fd, r.address, r.address_length, SOCK_CLOEXEC); break;
                    case Request::Kind::Connect: io_uring_prep_connect(sqe, r.fd, r.address, r.connect_length); break;
                    case Request::Kind::Cancel: io_uring_prep_cancel_fd(sqe, r.fd, IORING_ASYNC_CANCEL_ALL); break;
                }
                io_uring_sqe_set_data(sqe, new Op{std::move(r.done)});
            }

            void run() {
                if (!init_ring() || !opcodes_supported()) {
                    io_uring_queue_exit(&ring_);
                    finish_init(false);
                    return;
                }
                finish_init(true);
                arm_wake();
                std::vector<Request> batch;
                for (;;) {
                    {
                        std::lock_guard lock(queue_mutex_);
                        batch.swap(queue_);
                        queued_.store(0, std::memory_order_release);
                    }
                    for (Request &request : batch) prepare(request);
                    batch.clear();

                    // Under load the next request or completion is microseconds away: look for it briefly before paying for a sleep
                    // (and for the eventfd write every enqueue would otherwise owe a sleeping ring thread).
                    if (spin_for_work()) continue;

                    // About to sleep: say so first, then look at the queue once more, so a request that arrives in between is either
                    // seen here or wakes us through the eventfd -- never lost.
                    sleeping_.store(true, std::memory_order_seq_cst);
                    bool pending = false;
                    {
                        std::lock_guard lock(queue_mutex_);
                        pending = !queue_.empty();
                    }
                    if (pending) {
                        sleeping_.store(false, std::memory_order_relaxed);
                        continue;
                    }
                    const int waited = io_uring_submit_and_wait(&ring_, 1); // submits the whole batch in one enter
                    sleeping_.store(false, std::memory_order_relaxed);
                    if (waited < 0 && waited != -EINTR && waited != -EAGAIN && waited != -EBUSY) {
                        std::fprintf(stderr, "io_uring (network): the ring failed (%d); sockets will not make progress\n", waited);
                        return;
                    }
                    reap();
                }
            }

            /// True when work showed up while spinning: a queued request, or a completion that is ready to reap.
            [[nodiscard]] bool spin_for_work() {
                constexpr int kSpins = 400;
                for (int i = 0; i < kSpins; ++i) {
                    if (queued_.load(std::memory_order_acquire) != 0) return true;
                    if (io_uring_cq_ready(&ring_) != 0) {
                        reap();
                        return true;
                    }
                    if ((i & 31) == 31) {
                        // Deferred task-run mode finishes the kernel's side of a completion only when asked.
                        (void)io_uring_submit_and_get_events(&ring_);
                        if (io_uring_cq_ready(&ring_) != 0) {
                            reap();
                            return true;
                        }
                    }
#if defined(__x86_64__)
                    __builtin_ia32_pause();
#elif defined(__aarch64__)
                    __asm__ volatile("yield");
#endif
                }
                return false;
            }

            void reap() {
                io_uring_cqe *cqe = nullptr;
                unsigned head = 0;
                unsigned count = 0;
                bool rearm = false;
                io_uring_for_each_cqe(&ring_, head, cqe) {
                    ++count;
                    if (io_uring_cqe_get_data64(cqe) == kWakeTag) {
                        rearm = true;
                        continue;
                    }
                    std::unique_ptr<Op> op(static_cast<Op *>(io_uring_cqe_get_data(cqe)));
                    if (op) op->done(cqe->res);
                }
                io_uring_cq_advance(&ring_, count);
                if (rearm) arm_wake();
            }

            io_uring ring_{};
            std::uint64_t wake_value_ = 0;
            int wake_fd_ = -1;
            std::mutex queue_mutex_;
            std::vector<Request> queue_;
            std::atomic<bool> sleeping_{false};
            std::atomic<std::size_t> queued_{0}; ///< requests waiting in queue_ (readable without the lock)
            std::mutex init_mutex_;
            std::condition_variable init_done_;
            bool init_finished_ = false;
            bool ready_ = false;
        };

        /// A few rings, each with its own thread, a socket always on the same one (by descriptor number): the kernel does the data copy
        /// for sends and receives on the ring's thread, so a single ring would cap all sockets at one core's copy bandwidth.
        /// Never destroyed: sockets are closed by reactor threads that can still be running while the process exits, and a ring torn
        /// down by static destruction would turn their last `cancel` into a use of freed state. The kernel reclaims it at exit.
        struct Shards {
            std::vector<std::unique_ptr<Ring>> rings;

            Shards() {
                unsigned count = std::clamp(std::thread::hardware_concurrency() / 4, 1u, 8u);
                if (const char *forced = std::getenv("STURDY_ASYNC_NET_RINGS")) count = std::clamp(static_cast<unsigned>(std::atoi(forced)), 1u, 64u);
                for (unsigned i = 0; i < count; ++i) {
                    auto ring = std::make_unique<Ring>();
                    if (!ring->ready()) { // the first failure is the machine's answer: no partial sets
                        rings.clear();
                        return;
                    }
                    rings.push_back(std::move(ring));
                }
            }
        };

        Shards &shards() {
            static Shards *const instance = new Shards;
            return *instance;
        }

        Ring &ring_for(int fd) {
            auto &rings = shards().rings;
            return *rings[static_cast<std::size_t>(fd) % rings.size()];
        }

    } // namespace

    bool available() noexcept { return !shards().rings.empty(); }

    void recv(int fd, void *buffer, std::size_t size, Completion done) {
        Request r;
        r.kind = Request::Kind::Recv;
        r.fd = fd;
        r.buffer = buffer;
        r.size = size;
        r.done = std::move(done);
        ring_for(fd).enqueue(std::move(r));
    }

    void send(int fd, const void *buffer, std::size_t size, Completion done) {
        Request r;
        r.kind = Request::Kind::Send;
        r.fd = fd;
        r.buffer = const_cast<void *>(buffer);
        r.size = size;
        r.done = std::move(done);
        ring_for(fd).enqueue(std::move(r));
    }

    void recvmsg(int fd, msghdr *message, Completion done) {
        Request r;
        r.kind = Request::Kind::RecvMsg;
        r.fd = fd;
        r.message = message;
        r.done = std::move(done);
        ring_for(fd).enqueue(std::move(r));
    }

    void sendmsg(int fd, const msghdr *message, Completion done) {
        Request r;
        r.kind = Request::Kind::SendMsg;
        r.fd = fd;
        r.message = const_cast<msghdr *>(message);
        r.done = std::move(done);
        ring_for(fd).enqueue(std::move(r));
    }

    void accept(int fd, sockaddr *address, unsigned *length, Completion done) {
        Request r;
        r.kind = Request::Kind::Accept;
        r.fd = fd;
        r.address = address;
        r.address_length = length;
        r.done = std::move(done);
        ring_for(fd).enqueue(std::move(r));
    }

    void connect(int fd, const sockaddr *address, unsigned length, Completion done) {
        Request r;
        r.kind = Request::Kind::Connect;
        r.fd = fd;
        r.address = const_cast<sockaddr *>(address);
        r.connect_length = length;
        r.done = std::move(done);
        ring_for(fd).enqueue(std::move(r));
    }

    void cancel(int fd) noexcept {
        if (!available()) return;
        // The caller closes the descriptor right after this returns, and the kernel looks the number up when the cancel is
        // processed -- so wait for that, or the cancel could find a closed (or already reused) descriptor and the operations in
        // flight would hold the socket open forever. The ring thread answers in microseconds; the timeout only guards a dead ring.
        auto processed = std::make_shared<std::promise<void>>();
        auto signal = processed->get_future();
        Request r;
        r.kind = Request::Kind::Cancel;
        r.fd = fd;
        r.done = [processed](int) { processed->set_value(); };
        ring_for(fd).enqueue(std::move(r));
        (void)signal.wait_for(std::chrono::seconds(2));
    }

} // namespace SFT::Async::Detail::netring

#else

namespace SFT::Async::Detail::netring {
    bool available() noexcept { return false; }
    void recv(int, void *, std::size_t, Completion done) { done(-ENOSYS); }
    void send(int, const void *, std::size_t, Completion done) { done(-ENOSYS); }
    void recvmsg(int, msghdr *, Completion done) { done(-ENOSYS); }
    void sendmsg(int, const msghdr *, Completion done) { done(-ENOSYS); }
    void accept(int, sockaddr *, unsigned *, Completion done) { done(-ENOSYS); }
    void connect(int, const sockaddr *, unsigned, Completion done) { done(-ENOSYS); }
    void cancel(int) noexcept {}
} // namespace SFT::Async::Detail::netring

#endif
