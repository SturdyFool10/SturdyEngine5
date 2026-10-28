#include <Async/Uring.hpp>

#include <cerrno>

#if defined(STURDY_ASYNC_IO_URING)

#include <liburing.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

namespace SFT::Async::Detail::uring {

    namespace {

        struct Op {
            Completion done;
        };

        class Ring {
          public:
            Ring() {
                if (std::getenv("STURDY_ASYNC_NO_IO_URING") != nullptr) return;
                if (io_uring_queue_init(256, &ring_, 0) != 0) return;
                completion_fd_ = ::eventfd(0, EFD_CLOEXEC);
                stop_fd_ = ::eventfd(0, EFD_CLOEXEC);
                if (completion_fd_ < 0 || stop_fd_ < 0 || io_uring_register_eventfd(&ring_, completion_fd_) != 0) {
                    io_uring_queue_exit(&ring_);
                    close_fds();
                    return;
                }
                ready_ = true;
                reaper_ = std::thread([this] { reap(); });
            }

            ~Ring() {
                if (!ready_) return;
                const std::uint64_t one = 1;
                (void)!::write(stop_fd_, &one, sizeof one);
                reaper_.join();
                io_uring_queue_exit(&ring_);
                close_fds();
            }

            [[nodiscard]] bool ready() const noexcept { return ready_; }

            template <typename Prep>
            void submit(Completion done, Prep &&prepare) {
                auto op = std::make_unique<Op>(Op{std::move(done)});
                {
                    std::lock_guard lock(mutex_);
                    io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
                    if (sqe == nullptr) {
                        // Submission queue full: flush it to the kernel and try once more.
                        io_uring_submit(&ring_);
                        sqe = io_uring_get_sqe(&ring_);
                    }
                    if (sqe != nullptr) {
                        prepare(sqe);
                        io_uring_sqe_set_data(sqe, op.get());
                        if (io_uring_submit(&ring_) >= 0) {
                            op.release(); // owned by the kernel until the completion arrives
                            return;
                        }
                    }
                }
                op->done(-EAGAIN); // could not queue at all: report it like any failed operation
            }

          private:
            void close_fds() {
                if (completion_fd_ >= 0) ::close(completion_fd_);
                if (stop_fd_ >= 0) ::close(stop_fd_);
            }

            void reap() {
                pollfd fds[2] = {{completion_fd_, POLLIN, 0}, {stop_fd_, POLLIN, 0}};
                for (;;) {
                    if (::poll(fds, 2, -1) < 0 && errno != EINTR) return;
                    if ((fds[1].revents & POLLIN) != 0) return;
                    std::uint64_t counter = 0;
                    (void)!::read(completion_fd_, &counter, sizeof counter);
                    for (;;) {
                        long long result = 0;
                        std::unique_ptr<Op> op;
                        {
                            std::lock_guard lock(mutex_);
                            io_uring_cqe *cqe = nullptr;
                            if (io_uring_peek_cqe(&ring_, &cqe) != 0 || cqe == nullptr) break;
                            result = cqe->res;
                            op.reset(static_cast<Op *>(io_uring_cqe_get_data(cqe)));
                            io_uring_cqe_seen(&ring_, cqe);
                        }
                        if (op) op->done(result);
                    }
                }
            }

            io_uring ring_{};
            std::mutex mutex_;
            std::thread reaper_;
            int completion_fd_ = -1;
            int stop_fd_ = -1;
            bool ready_ = false;
        };

        Ring &ring() {
            static Ring instance;
            return instance;
        }

    } // namespace

    bool available() noexcept { return ring().ready(); }

    void read(int fd, void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        ring().submit(std::move(done), [=](io_uring_sqe *sqe) { io_uring_prep_read(sqe, fd, buffer, static_cast<unsigned>(size), offset); });
    }

    void write(int fd, const void *buffer, std::size_t size, std::uint64_t offset, Completion done) {
        ring().submit(std::move(done), [=](io_uring_sqe *sqe) { io_uring_prep_write(sqe, fd, buffer, static_cast<unsigned>(size), offset); });
    }

    void fsync(int fd, bool data_only, Completion done) {
        ring().submit(std::move(done), [=](io_uring_sqe *sqe) { io_uring_prep_fsync(sqe, fd, data_only ? IORING_FSYNC_DATASYNC : 0u); });
    }

} // namespace SFT::Async::Detail::uring

#else

namespace SFT::Async::Detail::uring {
    bool available() noexcept { return false; }
    void read(int, void *, std::size_t, std::uint64_t, Completion done) { done(-ENOSYS); }
    void write(int, const void *, std::size_t, std::uint64_t, Completion done) { done(-ENOSYS); }
    void fsync(int, bool, Completion done) { done(-ENOSYS); }
} // namespace SFT::Async::Detail::uring

#endif
