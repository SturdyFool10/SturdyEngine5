#pragma once

// Private to Async .cpp files: the reactor-side state behind TcpConnection, shared by the TCP, WebSocket and
// HTTP implementations.

#include <Async/Net.hpp>
#include <Async/NetInternal.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>

namespace SFT::Async {
    namespace Detail {

        using Strand = asio::strand<asio::io_context::executor_type>;
        using Bytes = std::vector<std::byte>;

        constexpr std::size_t kReadChunk = 64 * 1024;
        constexpr std::size_t kWriteChunk = 64 * 1024;
        constexpr auto kDefaultConnectTimeout = std::chrono::seconds(30);

        inline IoError closed_error(const char *what = "operation on a closed object") { return IoError{IoErrorCode::Closed, what}; }

        inline SocketAddress to_address(const tcp::endpoint &endpoint) { return {endpoint.address().to_string(), endpoint.port()}; }
        inline SocketAddress to_address(const udp::endpoint &endpoint) { return {endpoint.address().to_string(), endpoint.port()}; }

        template <typename T>
        TaskHandle<Result<T>> failed(IoError error) {
            Promise<Result<T>> promise;
            (void)promise.set_value(std::unexpected(std::move(error)));
            return promise.handle();
        }

        template <typename T>
        TaskHandle<Result<T>> cancelled_handle() {
            Promise<Result<T>> promise;
            (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            return promise.handle();
        }

        /// Cached TLS client contexts: building one loads the system trust store, which is slow.
        inline std::shared_ptr<asio::ssl::context> cached_client_context(const TlsClientOptions &options, IoError *error) {
            static std::mutex mutex;
            static std::map<std::string, std::shared_ptr<asio::ssl::context>> cache;
            std::string key = options.use_system_roots ? "1" : "0";
            key += '\n' + options.ca_certificates_pem + '\n' + options.client_certificate_chain_pem + '\n' + options.client_private_key_pem;
            key += options.identify_peer_by_certificate_only ? "\nidentity-only" : "\nverified";
            for (const auto &pin : options.pinned_sha256) key += "\np" + lowercase(pin);
            for (const auto &protocol : options.alpn) key += "\na" + protocol;
            {
                std::lock_guard lock(mutex);
                if (auto it = cache.find(key); it != cache.end()) return it->second;
            }
            auto context = make_tls_client_context(options);
            if (!context) {
                *error = context.error();
                return nullptr;
            }
            std::lock_guard lock(mutex);
            if (cache.size() >= 32) cache.clear();
            cache[key] = *context;
            return *context;
        }

        // =========================================================================================
        // TCP stream core
        // =========================================================================================
        struct ReadWaiter {
            enum class Kind { Some, Exact, Until };
            Kind kind = Kind::Some;
            std::size_t limit = 0;
            Bytes delimiter;
            Bytes accumulated;
            Promise<Result<Bytes>> promise;
            /// Native consumers (the Beast stream adapter) take their result by callback instead of a handle.
            std::function<void(Result<Bytes>)> callback;
            bool done = false;
            CancellationRegistration cancel_registration;

            void complete(Result<Bytes> result) {
                if (callback) {
                    callback(std::move(result));
                } else {
                    (void)promise.set_value(std::move(result));
                }
            }
        };

        struct WriteOp {
            enum class Kind { Data, Shutdown };
            Kind kind = Kind::Data;
            Bytes data;
            std::size_t offset = 0;
            bool started = false;
            bool done = false;
            std::atomic<bool> cancel_requested{false};
            std::function<void(Result<std::size_t>)> finish;
            std::function<void()> on_cancelled;
            CancellationRegistration cancel_registration;
        };

        struct StreamCore : std::enable_shared_from_this<StreamCore> {
            Strand strand{asio::make_strand(reactor())};
            tcp::socket socket{reactor()};
            std::shared_ptr<asio::ssl::context> tls_context;
            std::unique_ptr<asio::ssl::stream<tcp::socket &>> tls;
            std::atomic<bool> open{true};
            SocketAddress local;
            SocketAddress peer;
            TlsInfo tls_details;

            // Read side (strand-confined).
            Bytes read_buffer;
            std::size_t read_position = 0;
            Bytes read_chunk;
            bool read_in_flight = false;
            bool eof = false;
            std::optional<IoError> terminal_error;
            std::deque<std::shared_ptr<ReadWaiter>> waiters;

            // Write side (strand-confined).
            std::deque<std::shared_ptr<WriteOp>> write_queue;
            bool writing = false;

            StreamCore() = default;
            explicit StreamCore(tcp::socket &&accepted) : socket(std::move(accepted)) {}

            static TcpConnection wrap(std::shared_ptr<StreamCore> core) { return TcpConnection(std::move(core)); }
            /// Takes the transport out of a connection object (which is left empty and no longer closes it).
            static std::shared_ptr<StreamCore> take(TcpConnection &&connection) { return std::move(connection.core_); }

            void capture_addresses() {
                ErrorCode ec;
                const auto local_endpoint = socket.local_endpoint(ec);
                if (!ec) local = to_address(local_endpoint);
                const auto remote_endpoint = socket.remote_endpoint(ec);
                if (!ec) peer = to_address(remote_endpoint);
            }

            // ---- reading ---------------------------------------------------------------------
            void compact() {
                if (read_position == read_buffer.size()) {
                    read_buffer.clear();
                    read_position = 0;
                } else if (read_position >= kReadChunk) {
                    read_buffer.erase(read_buffer.begin(), read_buffer.begin() + static_cast<std::ptrdiff_t>(read_position));
                    read_position = 0;
                }
            }

            [[nodiscard]] std::size_t available() const noexcept { return read_buffer.size() - read_position; }

            /// Tries to finish `w` from buffered bytes. True when `w` was completed (successfully or not).
            bool try_satisfy(ReadWaiter &w) {
                switch (w.kind) {
                    case ReadWaiter::Kind::Some: {
                        if (available() == 0) return false;
                        const std::size_t take = (std::min)(available(), w.limit);
                        Bytes out(read_buffer.begin() + static_cast<std::ptrdiff_t>(read_position),
                                  read_buffer.begin() + static_cast<std::ptrdiff_t>(read_position + take));
                        read_position += take;
                        compact();
                        w.complete(Result<Bytes>(std::move(out)));
                        return true;
                    }
                    case ReadWaiter::Kind::Exact: {
                        const std::size_t take = (std::min)(available(), w.limit - w.accumulated.size());
                        w.accumulated.insert(w.accumulated.end(), read_buffer.begin() + static_cast<std::ptrdiff_t>(read_position),
                                             read_buffer.begin() + static_cast<std::ptrdiff_t>(read_position + take));
                        read_position += take;
                        compact();
                        if (w.accumulated.size() < w.limit) return false;
                        w.complete(Result<Bytes>(std::move(w.accumulated)));
                        return true;
                    }
                    case ReadWaiter::Kind::Until: {
                        while (read_position < read_buffer.size()) {
                            w.accumulated.push_back(read_buffer[read_position++]);
                            if (w.accumulated.size() >= w.delimiter.size() &&
                                std::equal(w.delimiter.begin(), w.delimiter.end(), w.accumulated.end() - static_cast<std::ptrdiff_t>(w.delimiter.size()))) {
                                compact();
                                w.complete(Result<Bytes>(std::move(w.accumulated)));
                                return true;
                            }
                            if (w.accumulated.size() >= w.limit) {
                                compact();
                                w.complete(Result<Bytes>(std::unexpected(
                                    IoError{IoErrorCode::LimitExceeded, "delimiter not found within the byte limit"})));
                                return true;
                            }
                        }
                        compact();
                        return false;
                    }
                }
                return false;
            }

            void finish_with_terminal(ReadWaiter &w) {
                if (terminal_error) {
                    w.complete(Result<Bytes>(std::unexpected(*terminal_error)));
                } else if (w.kind == ReadWaiter::Kind::Some) {
                    w.complete(Result<Bytes>(Bytes{}));
                } else {
                    w.complete(Result<Bytes>(std::unexpected(IoError{
                        IoErrorCode::UnexpectedEof, "connection closed after " + std::to_string(w.accumulated.size()) + " bytes"})));
                }
            }

            void pump_reads() {
                while (!waiters.empty()) {
                    auto w = waiters.front();
                    if (w->done) {
                        waiters.pop_front();
                        continue;
                    }
                    if (try_satisfy(*w)) {
                        w->done = true;
                        w->cancel_registration.reset();
                        waiters.pop_front();
                        continue;
                    }
                    if (eof || terminal_error) {
                        w->done = true;
                        w->cancel_registration.reset();
                        waiters.pop_front();
                        finish_with_terminal(*w);
                        continue;
                    }
                    break;
                }
                const bool wanted = std::any_of(waiters.begin(), waiters.end(), [](const auto &w) { return !w->done; });
                if (wanted && !read_in_flight && !eof && !terminal_error && open.load(std::memory_order_acquire)) {
                    start_read();
                }
            }

            void start_read() {
                read_in_flight = true;
                read_chunk.resize(kReadChunk);
                auto handler = asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec, std::size_t n) {
                    self->on_read(ec, n);
                });
                if (tls) {
                    tls->async_read_some(asio::buffer(read_chunk), std::move(handler));
                } else {
                    socket.async_read_some(asio::buffer(read_chunk), std::move(handler));
                }
            }

            void on_read(const ErrorCode &ec, std::size_t n) {
                read_in_flight = false;
                if (n > 0) {
                    read_buffer.insert(read_buffer.end(), read_chunk.begin(), read_chunk.begin() + static_cast<std::ptrdiff_t>(n));
                }
                if (ec == asio::error::operation_aborted && open.load(std::memory_order_acquire)) {
                    // Something else (e.g. a TLS shutdown timeout) aborted the socket; we are still open, so go on.
                } else if (ec == asio::error::eof) {
                    eof = true;
                } else if (ec) {
                    terminal_error = open.load(std::memory_order_acquire) ? to_io_error("receive", ec) : closed_error();
                }
                if (waiters.empty() || std::all_of(waiters.begin(), waiters.end(), [](const auto &w) { return w->done; })) {
                    read_chunk.clear();
                    read_chunk.shrink_to_fit();
                }
                pump_reads();
            }

            void add_waiter(std::shared_ptr<ReadWaiter> w, const CancellationToken &token) {
                if (token.can_be_cancelled()) {
                    w->cancel_registration = token.on_cancel([weak_core = weak_from_this(), weak_waiter = std::weak_ptr<ReadWaiter>(w)] {
                        auto core = weak_core.lock();
                        if (!core) return;
                        asio::post(core->strand, [core, weak_waiter] {
                            if (auto waiter = weak_waiter.lock(); waiter && !waiter->done) {
                                waiter->done = true;
                                (void)waiter->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                            }
                        });
                    });
                }
                waiters.push_back(std::move(w));
                pump_reads();
            }

            // ---- writing ---------------------------------------------------------------------
            void pump_writes() {
                if (writing) return;
                while (!write_queue.empty()) {
                    auto op = write_queue.front();
                    if (op->done) {
                        write_queue.pop_front();
                        continue;
                    }
                    if (!open.load(std::memory_order_acquire)) {
                        write_queue.pop_front();
                        complete_write(*op, std::unexpected(closed_error()));
                        continue;
                    }
                    if (op->kind == WriteOp::Kind::Shutdown) {
                        writing = true;
                        start_shutdown(op);
                        return;
                    }
                    if (op->data.empty()) {
                        write_queue.pop_front();
                        complete_write(*op, Result<std::size_t>(0));
                        continue;
                    }
                    op->started = true;
                    writing = true;
                    write_chunk(op);
                    return;
                }
            }

            void complete_write(WriteOp &op, Result<std::size_t> result) {
                if (op.done) return;
                op.done = true;
                op.cancel_registration.reset();
                op.finish(std::move(result));
            }

            void write_chunk(const std::shared_ptr<WriteOp> &op) {
                const std::size_t count = (std::min)(op->data.size() - op->offset, kWriteChunk);
                auto handler = asio::bind_executor(strand, [self = shared_from_this(), op](const ErrorCode &ec, std::size_t written) {
                    self->on_write(op, ec, written);
                });
                if (tls) {
                    tls->async_write_some(asio::buffer(op->data.data() + op->offset, count), std::move(handler));
                } else {
                    socket.async_write_some(asio::buffer(op->data.data() + op->offset, count), std::move(handler));
                }
            }

            void on_write(const std::shared_ptr<WriteOp> &op, const ErrorCode &ec, std::size_t written) {
                auto pop_op = [&] {
                    if (!write_queue.empty() && write_queue.front() == op) write_queue.pop_front();
                    writing = false;
                };
                if (op->done) { // failed by close() while the chunk was in flight
                    pop_op();
                    pump_writes();
                    return;
                }
                if (ec) {
                    pop_op();
                    complete_write(*op, std::unexpected(open.load(std::memory_order_acquire) ? to_io_error("send", ec) : closed_error()));
                    pump_writes();
                    return;
                }
                op->offset += written;
                if (op->offset == op->data.size()) {
                    pop_op();
                    complete_write(*op, Result<std::size_t>(op->data.size()));
                } else if (op->cancel_requested.load(std::memory_order_acquire)) {
                    pop_op();
                    op->done = true;
                    op->cancel_registration.reset();
                    op->on_cancelled();
                } else {
                    write_chunk(op);
                    return;
                }
                pump_writes();
            }

            void start_shutdown(const std::shared_ptr<WriteOp> &op) {
                auto finish_shutdown = [self = shared_from_this(), op](Result<std::size_t> result) {
                    if (!self->write_queue.empty() && self->write_queue.front() == op) self->write_queue.pop_front();
                    self->writing = false;
                    self->complete_write(*op, std::move(result));
                    self->pump_writes();
                };
                if (!tls) {
                    ErrorCode ec;
                    socket.shutdown(tcp::socket::shutdown_send, ec);
                    finish_shutdown(ec ? Result<std::size_t>(std::unexpected(to_io_error("shutdown", ec))) : Result<std::size_t>(0));
                    return;
                }
                // TLS close_notify; the peer's reply is not awaited for long, or a silent peer would hang us.
                auto timer = std::make_shared<asio::steady_timer>(strand);
                auto finished = std::make_shared<bool>(false);
                timer->expires_after(std::chrono::seconds(2));
                timer->async_wait([finished, finish_shutdown](const ErrorCode &ec) {
                    if (ec || *finished) return;
                    *finished = true;
                    finish_shutdown(Result<std::size_t>(0));
                });
                tls->async_shutdown(asio::bind_executor(strand, [self = shared_from_this(), timer, finished, finish_shutdown](const ErrorCode &ec) {
                    if (*finished) return;
                    *finished = true;
                    timer->cancel();
                    // Both close_notify records exchanged (ok / eof) or the peer vanished: nothing more will be read.
                    if (!ec || ec == asio::error::eof || ec == asio::ssl::error::stream_truncated) self->eof = true;
                    finish_shutdown(Result<std::size_t>(0));
                    self->pump_reads();
                }));
            }

            void add_write(std::shared_ptr<WriteOp> op, const CancellationToken &token) {
                if (token.can_be_cancelled()) {
                    op->cancel_registration = token.on_cancel([weak_core = weak_from_this(), weak_op = std::weak_ptr<WriteOp>(op)] {
                        auto core = weak_core.lock();
                        auto pending = weak_op.lock();
                        if (!core || !pending) return;
                        pending->cancel_requested.store(true, std::memory_order_release);
                        asio::post(core->strand, [core, pending] {
                            if (pending->done || pending->started) return; // started writes stop after their current chunk
                            pending->done = true;
                            std::erase(core->write_queue, pending);
                            pending->on_cancelled();
                        });
                    });
                }
                write_queue.push_back(std::move(op));
                pump_writes();
            }

            // ---- closing ---------------------------------------------------------------------
            void close_now() {
                if (!open.exchange(false, std::memory_order_acq_rel)) return;
                ErrorCode ec;
                socket.shutdown(tcp::socket::shutdown_both, ec);
                socket.close(ec);
                terminal_error = closed_error();
                for (auto &w : waiters) {
                    if (!w->done) {
                        w->done = true;
                        w->cancel_registration.reset();
                        w->complete(Result<Bytes>(std::unexpected(closed_error())));
                    }
                }
                waiters.clear();
                auto queued = std::move(write_queue);
                write_queue.clear();
                for (auto &op : queued) {
                    complete_write(*op, std::unexpected(closed_error()));
                }
            }

            void request_close() {
                asio::post(strand, [self = shared_from_this()] { self->close_now(); });
            }
        };
    } // namespace Detail
} // namespace SFT::Async
