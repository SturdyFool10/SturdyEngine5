#include <Async/Net.hpp>
#include <Async/Promise.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <Async/NetCore.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>

namespace SFT::Async {

    // =============================================================================================
    // Shared helpers
    // =============================================================================================
    namespace Detail {


        // =========================================================================================
        // Connecting
        // =========================================================================================
        struct ConnectOp : std::enable_shared_from_this<ConnectOp> {
            std::shared_ptr<StreamCore> core = std::make_shared<StreamCore>();
            tcp::resolver resolver{reactor()};
            asio::steady_timer timer{reactor()};
            Promise<Result<TcpConnection>> promise;
            std::string host;
            u16 port = 0;
            bool use_tls = false;
            TlsClientOptions tls_options;
            std::chrono::milliseconds timeout{};
            CancellationRegistration cancel_registration;
            bool finished = false;

            void start(const CancellationToken &token) {
                if (token.can_be_cancelled()) {
                    cancel_registration = token.on_cancel([weak = weak_from_this()] {
                        if (auto self = weak.lock()) {
                            asio::post(self->core->strand, [self] { self->finish_cancelled(); });
                        }
                    });
                }
                asio::post(core->strand, [self = shared_from_this()] { self->begin(); });
            }

            void begin() {
                if (finished) return;
                if (use_tls) {
                    IoError error{IoErrorCode::Unknown, ""};
                    core->tls_context = cached_client_context(tls_options, &error);
                    if (!core->tls_context) return fail(std::move(error));
                }
                timer.expires_after(timeout.count() > 0 ? timeout : std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultConnectTimeout));
                timer.async_wait(asio::bind_executor(core->strand, [self = shared_from_this()](const ErrorCode &ec) {
                    if (!ec) self->fail(IoError{IoErrorCode::TimedOut, "connect timed out"});
                }));
                resolver.async_resolve(host, std::to_string(port),
                                       asio::bind_executor(core->strand, [self = shared_from_this()](const ErrorCode &ec, tcp::resolver::results_type results) {
                                           if (self->finished) return;
                                           if (ec) return self->fail(to_io_error(("resolve '" + self->host + "'").c_str(), ec));
                                           asio::async_connect(self->core->socket, results,
                                                               asio::bind_executor(self->core->strand, [self](const ErrorCode &error, const tcp::endpoint &) {
                                                                   if (self->finished) return;
                                                                   if (error) return self->fail(to_io_error("connect", error));
                                                                   self->connected();
                                                               }));
                                       }));
            }

            void connected() {
                ErrorCode ignored;
                core->socket.set_option(tcp::no_delay(true), ignored);
                core->capture_addresses();
                if (!use_tls) return succeed();
                core->tls = std::make_unique<asio::ssl::stream<tcp::socket &>>(core->socket, *core->tls_context);
                if (auto prepared = prepare_tls_client(core->tls->native_handle(), tls_options, host); !prepared) {
                    return fail(prepared.error());
                }
                core->tls->async_handshake(asio::ssl::stream_base::client,
                                           asio::bind_executor(core->strand, [self = shared_from_this()](const ErrorCode &ec) {
                                               if (self->finished) return;
                                               if (ec) return self->fail(to_io_error("TLS handshake", ec));
                                               if (auto pinned = check_certificate_pin(self->core->tls->native_handle(), self->tls_options.pinned_sha256); !pinned) {
                                                   return self->fail(pinned.error());
                                               }
                                               self->core->tls_details = describe_tls(self->core->tls->native_handle());
                                               self->succeed();
                                           }));
            }

            void teardown() {
                finished = true;
                cancel_registration.reset();
                timer.cancel();
                resolver.cancel();
            }

            void succeed() {
                teardown();
                (void)promise.set_value(Result<TcpConnection>(StreamCore::wrap(core)));
            }

            void fail(IoError error) {
                if (finished) return;
                teardown();
                core->close_now();
                (void)promise.set_value(Result<TcpConnection>(std::unexpected(std::move(error))));
            }

            void finish_cancelled() {
                if (finished) return;
                teardown();
                core->close_now();
                (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            }
        };

        TaskHandle<Result<TcpConnection>> start_connect(std::string host, u16 port, bool use_tls, TlsClientOptions tls,
                                                        std::chrono::milliseconds timeout, CancellationToken cancellation) {
            if (host.empty()) {
                return failed<TcpConnection>(IoError{IoErrorCode::InvalidArgument, "connect() needs a host"});
            }
            if (cancellation.is_cancelled()) return cancelled_handle<TcpConnection>();
            auto op = std::make_shared<ConnectOp>();
            op->host = std::move(host);
            op->port = port;
            op->use_tls = use_tls;
            op->tls_options = std::move(tls);
            op->timeout = timeout;
            auto handle = op->promise.handle();
            op->start(cancellation);
            return handle;
        }

        // =========================================================================================
        // Listener core
        // =========================================================================================
        struct AcceptWaiter {
            Promise<Result<TcpConnection>> promise;
            bool done = false;
            CancellationRegistration cancel_registration;
        };

        struct ListenerCore : std::enable_shared_from_this<ListenerCore> {
            static constexpr std::size_t kReadyCap = 128;
            static constexpr std::size_t kHandshakeCap = 256;

            Strand strand{asio::make_strand(reactor())};
            tcp::acceptor acceptor{reactor()};
            std::shared_ptr<asio::ssl::context> tls_context;
            TlsServerOptions tls_options;
            bool use_tls = false;
            std::atomic<bool> open{true};
            SocketAddress local;

            std::deque<std::shared_ptr<StreamCore>> ready;
            std::deque<std::shared_ptr<AcceptWaiter>> waiters;
            std::size_t handshaking = 0;
            bool accepting = false;
            std::optional<IoError> accept_error;

            static TcpListener wrap(std::shared_ptr<ListenerCore> core) { return TcpListener(std::move(core)); }

            void accept_loop() {
                if (!open.load(std::memory_order_acquire) || accepting) return;
                if (ready.size() + handshaking >= kReadyCap || handshaking >= kHandshakeCap) return;
                accepting = true;
                acceptor.async_accept(reactor(), asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec, tcp::socket socket) {
                    self->accepting = false;
                    if (!self->open.load(std::memory_order_acquire)) return;
                    if (ec) {
                        // Transient failures (EMFILE, ECONNABORTED...) are retried after a pause; waiters learn of them.
                        if (ec != asio::error::operation_aborted) {
                            self->accept_error = to_io_error("accept", ec);
                            self->deliver();
                            auto timer = std::make_shared<asio::steady_timer>(self->strand, std::chrono::milliseconds(50));
                            timer->async_wait([self, timer](const ErrorCode &) { self->accept_loop(); });
                        }
                        return;
                    }
                    self->on_accepted(std::move(socket));
                    self->accept_loop();
                }));
            }

            void on_accepted(tcp::socket socket) {
                auto core = std::make_shared<StreamCore>(std::move(socket));
                ErrorCode ignored;
                core->socket.set_option(tcp::no_delay(true), ignored);
                core->capture_addresses();
                if (!use_tls) {
                    ready.push_back(std::move(core));
                    deliver();
                    return;
                }
                ++handshaking;
                core->tls_context = tls_context;
                core->tls = std::make_unique<asio::ssl::stream<tcp::socket &>>(core->socket, *tls_context);
                auto timer = std::make_shared<asio::steady_timer>(strand, std::chrono::milliseconds(tls_options.handshake_timeout_ms));
                auto settled = std::make_shared<bool>(false);
                timer->async_wait([core, settled](const ErrorCode &ec) {
                    if (!ec && !*settled) core->request_close(); // handshake timed out: closing aborts it
                });
                core->tls->async_handshake(asio::ssl::stream_base::server,
                                           asio::bind_executor(strand, [self = shared_from_this(), core, timer, settled](const ErrorCode &ec) {
                                               *settled = true;
                                               timer->cancel();
                                               --self->handshaking;
                                               bool good = !ec && self->open.load(std::memory_order_acquire);
                                               if (good) {
                                                   good = static_cast<bool>(check_certificate_pin(core->tls->native_handle(), self->tls_options.pinned_client_sha256));
                                               }
                                               if (good) {
                                                   core->tls_details = describe_tls(core->tls->native_handle());
                                                   self->ready.push_back(core);
                                                   self->deliver();
                                               } else {
                                                   core->close_now();
                                               }
                                               self->accept_loop();
                                           }));
            }

            void deliver() {
                while (!waiters.empty()) {
                    auto w = waiters.front();
                    if (w->done) {
                        waiters.pop_front();
                        continue;
                    }
                    if (!ready.empty()) {
                        auto core = std::move(ready.front());
                        ready.pop_front();
                        waiters.pop_front();
                        w->done = true;
                        w->cancel_registration.reset();
                        if (core->open.load(std::memory_order_acquire)) {
                            (void)w->promise.set_value(Result<TcpConnection>(StreamCore::wrap(std::move(core))));
                        } else {
                            waiters.push_front(w);
                            w->done = false;
                        }
                        continue;
                    }
                    if (accept_error) {
                        waiters.pop_front();
                        w->done = true;
                        w->cancel_registration.reset();
                        (void)w->promise.set_value(Result<TcpConnection>(std::unexpected(*accept_error)));
                        accept_error.reset();
                        continue;
                    }
                    break;
                }
                accept_loop();
            }

            void close_now() {
                if (!open.exchange(false, std::memory_order_acq_rel)) return;
                ErrorCode ec;
                acceptor.close(ec);
                for (auto &w : waiters) {
                    if (!w->done) {
                        w->done = true;
                        w->cancel_registration.reset();
                        (void)w->promise.set_value(Result<TcpConnection>(std::unexpected(closed_error("listener closed"))));
                    }
                }
                waiters.clear();
                for (auto &core : ready) core->close_now();
                ready.clear();
            }
        };

        // =========================================================================================
        // UDP core
        // =========================================================================================
        struct UdpWaiter {
            std::size_t max_bytes = 0;
            Promise<Result<UdpDatagram>> promise;
            bool done = false;
            CancellationRegistration cancel_registration;
        };

        struct UdpCore : std::enable_shared_from_this<UdpCore> {
            static constexpr std::size_t kStashCap = 64;

            Strand strand{asio::make_strand(reactor())};
            udp::socket socket{reactor()};
            udp::resolver resolver{reactor()};
            std::atomic<bool> open{true};
            SocketAddress local;

            Bytes receive_buffer;
            udp::endpoint sender;
            bool read_in_flight = false;
            std::deque<std::shared_ptr<UdpWaiter>> waiters;
            std::deque<UdpDatagram> stash;
            std::unordered_map<std::string, udp::endpoint> resolved;

            static UdpSocket wrap(std::shared_ptr<UdpCore> core) { return UdpSocket(std::move(core)); }

            void pump() {
                while (!waiters.empty() && waiters.front()->done) waiters.pop_front();
                while (!waiters.empty() && !stash.empty()) {
                    auto w = waiters.front();
                    waiters.pop_front();
                    UdpDatagram datagram = std::move(stash.front());
                    stash.pop_front();
                    if (datagram.data.size() > w->max_bytes) datagram.data.resize(w->max_bytes);
                    w->done = true;
                    w->cancel_registration.reset();
                    (void)w->promise.set_value(Result<UdpDatagram>(std::move(datagram)));
                    while (!waiters.empty() && waiters.front()->done) waiters.pop_front();
                }
                if (!waiters.empty() && !read_in_flight && open.load(std::memory_order_acquire)) {
                    read_in_flight = true;
                    receive_buffer.resize(65536);
                    socket.async_receive_from(asio::buffer(receive_buffer), sender,
                                              asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec, std::size_t n) {
                                                  self->read_in_flight = false;
                                                  if (ec) {
                                                      if (!self->open.load(std::memory_order_acquire)) return;
                                                      // ICMP-induced errors on connected sockets, aborted ops: report to one waiter and carry on.
                                                      if (ec != asio::error::operation_aborted) self->fail_front(to_io_error("receive_from", ec));
                                                      self->pump();
                                                      return;
                                                  }
                                                  UdpDatagram datagram;
                                                  datagram.data.assign(self->receive_buffer.begin(), self->receive_buffer.begin() + static_cast<std::ptrdiff_t>(n));
                                                  datagram.sender_host = self->sender.address().to_string();
                                                  datagram.sender_port = self->sender.port();
                                                  if (self->stash.size() >= kStashCap) self->stash.pop_front(); // UDP may drop; drop the oldest
                                                  self->stash.push_back(std::move(datagram));
                                                  self->pump();
                                              }));
                }
            }

            void fail_front(IoError error) {
                while (!waiters.empty() && waiters.front()->done) waiters.pop_front();
                if (waiters.empty()) return;
                auto w = waiters.front();
                waiters.pop_front();
                w->done = true;
                w->cancel_registration.reset();
                (void)w->promise.set_value(Result<UdpDatagram>(std::unexpected(std::move(error))));
            }

            void close_now() {
                if (!open.exchange(false, std::memory_order_acq_rel)) return;
                ErrorCode ec;
                socket.close(ec);
                resolver.cancel();
                for (auto &w : waiters) {
                    if (!w->done) {
                        w->done = true;
                        w->cancel_registration.reset();
                        (void)w->promise.set_value(Result<UdpDatagram>(std::unexpected(closed_error())));
                    }
                }
                waiters.clear();
            }
        };

    } // namespace Detail

    using namespace Detail;

    // =============================================================================================
    // Public API: reactor / resolving
    // =============================================================================================
    bool configure_network_reactor(unsigned thread_count) { return configure_reactor(thread_count); }

    TaskHandle<std::expected<std::vector<SocketAddress>, IoError>> resolve_host(std::string host, u16 port) {
        struct ResolveOp {
            tcp::resolver resolver{reactor()};
            Promise<Result<std::vector<SocketAddress>>> promise;
        };
        auto op = std::make_shared<ResolveOp>();
        auto handle = op->promise.handle();
        op->resolver.async_resolve(host, std::to_string(port), [op, host](const ErrorCode &ec, tcp::resolver::results_type results) {
            if (ec) {
                (void)op->promise.set_value(Result<std::vector<SocketAddress>>(std::unexpected(to_io_error(("resolve '" + host + "'").c_str(), ec))));
                return;
            }
            std::vector<SocketAddress> out;
            for (const auto &entry : results) out.push_back(to_address(entry.endpoint()));
            (void)op->promise.set_value(Result<std::vector<SocketAddress>>(std::move(out)));
        });
        return handle;
    }

    // =============================================================================================
    // TcpConnection
    // =============================================================================================
    TcpConnection::~TcpConnection() noexcept { close(); }

    TcpConnection &TcpConnection::operator=(TcpConnection &&other) noexcept {
        if (this != &other) {
            close();
            core_ = std::move(other.core_);
        }
        return *this;
    }

    TaskHandle<std::expected<TcpConnection, IoError>> TcpConnection::connect(std::string host, u16 port, std::chrono::milliseconds timeout,
                                                                             CancellationToken cancellation) {
        return start_connect(std::move(host), port, false, {}, timeout, std::move(cancellation));
    }

    TaskHandle<std::expected<TcpConnection, IoError>> TcpConnection::connect_tls(std::string host, u16 port, TlsClientOptions tls,
                                                                                 std::chrono::milliseconds timeout,
                                                                                 CancellationToken cancellation) {
        if (timeout.count() <= 0 && tls.handshake_timeout_ms > 0) {
            timeout = std::chrono::milliseconds(tls.handshake_timeout_ms) + std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultConnectTimeout);
        }
        return start_connect(std::move(host), port, true, std::move(tls), timeout, std::move(cancellation));
    }

    namespace {
        template <typename Core, typename T, typename Fn>
        TaskHandle<Result<T>> on_strand(const std::shared_ptr<Core> &core, Fn fn) {
            Promise<Result<T>> promise;
            auto handle = promise.handle();
            if (!core) {
                (void)promise.set_value(Result<T>(std::unexpected(closed_error())));
                return handle;
            }
            asio::post(core->strand, [core, promise, fn = std::move(fn)]() mutable {
                Promise<Result<T>> p = promise;
                (void)p.set_value(fn(*core));
            });
            return handle;
        }
    } // namespace

    TaskHandle<std::expected<std::size_t, IoError>> TcpConnection::send(std::vector<std::byte> data, CancellationToken cancellation) {
        if (!core_) return failed<std::size_t>(closed_error());
        if (cancellation.is_cancelled()) return cancelled_handle<std::size_t>();
        Promise<Result<std::size_t>> promise;
        auto op = std::make_shared<WriteOp>();
        op->data = std::move(data);
        op->finish = [promise](Result<std::size_t> result) mutable { (void)promise.set_value(std::move(result)); };
        op->on_cancelled = [promise]() mutable { (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{})); };
        auto handle = promise.handle();
        asio::post(core_->strand, [core = core_, op, cancellation] { core->add_write(op, cancellation); });
        return handle;
    }

    TaskHandle<std::expected<void, IoError>> TcpConnection::send_all(std::vector<std::byte> data, CancellationToken cancellation) {
        if (!core_) return failed<void>(closed_error());
        if (cancellation.is_cancelled()) return cancelled_handle<void>();
        Promise<Result<void>> promise;
        auto op = std::make_shared<WriteOp>();
        op->data = std::move(data);
        op->finish = [promise](Result<std::size_t> result) mutable {
            (void)promise.set_value(result ? Result<void>() : Result<void>(std::unexpected(std::move(result.error()))));
        };
        op->on_cancelled = [promise]() mutable { (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{})); };
        auto handle = promise.handle();
        asio::post(core_->strand, [core = core_, op, cancellation] { core->add_write(op, cancellation); });
        return handle;
    }

    namespace {
        TaskHandle<Result<Bytes>> queue_read(const std::shared_ptr<StreamCore> &core, std::shared_ptr<ReadWaiter> waiter,
                                             const CancellationToken &cancellation) {
            if (!core) return failed<Bytes>(closed_error());
            if (cancellation.is_cancelled()) return cancelled_handle<Bytes>();
            auto handle = waiter->promise.handle();
            asio::post(core->strand, [core, waiter, cancellation] { core->add_waiter(waiter, cancellation); });
            return handle;
        }
    } // namespace

    TaskHandle<std::expected<std::vector<std::byte>, IoError>> TcpConnection::receive(std::size_t max_bytes, CancellationToken cancellation) {
        if (max_bytes == 0) return failed<Bytes>(IoError{IoErrorCode::InvalidArgument, "receive() needs max_bytes > 0"});
        auto waiter = std::make_shared<ReadWaiter>();
        waiter->kind = ReadWaiter::Kind::Some;
        waiter->limit = max_bytes;
        return queue_read(core_, std::move(waiter), cancellation);
    }

    TaskHandle<std::expected<std::vector<std::byte>, IoError>> TcpConnection::receive_exact(std::size_t byte_count,
                                                                                            CancellationToken cancellation) {
        auto waiter = std::make_shared<ReadWaiter>();
        waiter->kind = ReadWaiter::Kind::Exact;
        waiter->limit = byte_count;
        if (byte_count == 0) {
            Promise<Result<Bytes>> done;
            (void)done.set_value(Result<Bytes>(Bytes{}));
            return done.handle();
        }
        return queue_read(core_, std::move(waiter), cancellation);
    }

    TaskHandle<std::expected<std::vector<std::byte>, IoError>> TcpConnection::receive_until(std::string delimiter, std::size_t max_bytes,
                                                                                            CancellationToken cancellation) {
        if (delimiter.empty() || max_bytes < delimiter.size()) {
            return failed<Bytes>(IoError{IoErrorCode::InvalidArgument, "receive_until() needs a delimiter that fits in max_bytes"});
        }
        auto waiter = std::make_shared<ReadWaiter>();
        waiter->kind = ReadWaiter::Kind::Until;
        waiter->limit = max_bytes;
        for (char c : delimiter) waiter->delimiter.push_back(static_cast<std::byte>(c));
        return queue_read(core_, std::move(waiter), cancellation);
    }

    namespace {
        /// A connection can change security only when nothing else is touching it.
        std::optional<IoError> quiet_connection_error(const StreamCore &core) {
            if (core.tls) return IoError{IoErrorCode::InvalidArgument, "connection already uses TLS"};
            if (core.read_in_flight || core.available() > 0 || !core.waiters.empty() || core.writing || !core.write_queue.empty()) {
                return IoError{IoErrorCode::InvalidArgument, "start_tls needs a quiet connection (no pending IO, no unread bytes)"};
            }
            return std::nullopt;
        }
    } // namespace

    TaskHandle<std::expected<void, IoError>> TcpConnection::start_tls_client(std::string host, TlsClientOptions tls, CancellationToken cancellation) {
        if (!core_) return failed<void>(closed_error());
        IoError context_error{IoErrorCode::Unknown, ""};
        auto context = cached_client_context(tls, &context_error);
        if (!context) return failed<void>(context_error);
        Promise<Result<void>> promise;
        auto handle = promise.handle();
        asio::post(core_->strand, [core = core_, context, host = std::move(host), tls = std::move(tls), promise, cancellation]() mutable {
            if (auto busy = quiet_connection_error(*core)) return (void)promise.set_value(Result<void>(std::unexpected(*busy)));
            if (cancellation.is_cancelled()) return (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            core->tls_context = context;
            core->tls = std::make_unique<asio::ssl::stream<tcp::socket &>>(core->socket, *context);
            auto abandon = [core, promise](IoError error) mutable {
                core->tls.reset();
                core->tls_context.reset();
                core->close_now();
                (void)promise.set_value(Result<void>(std::unexpected(std::move(error))));
            };
            if (auto prepared = prepare_tls_client(core->tls->native_handle(), tls, host); !prepared) return abandon(prepared.error());
            auto timer = std::make_shared<asio::steady_timer>(core->strand, std::chrono::milliseconds(tls.handshake_timeout_ms));
            auto settled = std::make_shared<bool>(false);
            timer->async_wait([core, settled](const ErrorCode &ec) {
                if (!ec && !*settled) core->request_close();
            });
            core->tls->async_handshake(asio::ssl::stream_base::client, asio::bind_executor(core->strand, [core, tls, timer, settled, promise, abandon](const ErrorCode &ec) mutable {
                *settled = true;
                timer->cancel();
                if (ec) return abandon(to_io_error("TLS handshake", ec));
                if (auto pinned = check_certificate_pin(core->tls->native_handle(), tls.pinned_sha256); !pinned) return abandon(pinned.error());
                core->tls_details = describe_tls(core->tls->native_handle());
                (void)promise.set_value(Result<void>());
            }));
        });
        return handle;
    }

    TaskHandle<std::expected<void, IoError>> TcpConnection::start_tls_server(TlsServerOptions tls) {
        if (!core_) return failed<void>(closed_error());
        auto context = make_tls_server_context(tls);
        if (!context) return failed<void>(context.error());
        Promise<Result<void>> promise;
        auto handle = promise.handle();
        asio::post(core_->strand, [core = core_, context = *context, tls = std::move(tls), promise]() mutable {
            if (auto busy = quiet_connection_error(*core)) return (void)promise.set_value(Result<void>(std::unexpected(*busy)));
            core->tls_context = context;
            core->tls = std::make_unique<asio::ssl::stream<tcp::socket &>>(core->socket, *context);
            auto timer = std::make_shared<asio::steady_timer>(core->strand, std::chrono::milliseconds(tls.handshake_timeout_ms));
            auto settled = std::make_shared<bool>(false);
            timer->async_wait([core, settled](const ErrorCode &ec) {
                if (!ec && !*settled) core->request_close();
            });
            core->tls->async_handshake(asio::ssl::stream_base::server, asio::bind_executor(core->strand, [core, tls, timer, settled, promise](const ErrorCode &ec) mutable {
                *settled = true;
                timer->cancel();
                auto fail = [&](IoError error) {
                    core->tls.reset();
                    core->tls_context.reset();
                    core->close_now();
                    (void)promise.set_value(Result<void>(std::unexpected(std::move(error))));
                };
                if (ec) return fail(to_io_error("TLS handshake", ec));
                if (auto pinned = check_certificate_pin(core->tls->native_handle(), tls.pinned_client_sha256); !pinned) return fail(pinned.error());
                core->tls_details = describe_tls(core->tls->native_handle());
                (void)promise.set_value(Result<void>());
            }));
        });
        return handle;
    }

    TaskHandle<std::expected<void, IoError>> TcpConnection::shutdown_write() {
        if (!core_) return failed<void>(closed_error());
        Promise<Result<void>> promise;
        auto op = std::make_shared<WriteOp>();
        op->kind = WriteOp::Kind::Shutdown;
        op->finish = [promise](Result<std::size_t> result) mutable {
            (void)promise.set_value(result ? Result<void>() : Result<void>(std::unexpected(std::move(result.error()))));
        };
        op->on_cancelled = [] {};
        auto handle = promise.handle();
        asio::post(core_->strand, [core = core_, op] { core->add_write(op, {}); });
        return handle;
    }

    TaskHandle<std::expected<void, IoError>> TcpConnection::set_nodelay(bool enabled) {
        return on_strand<StreamCore, void>(core_, [enabled](StreamCore &core) -> Result<void> {
            ErrorCode ec;
            core.socket.set_option(tcp::no_delay(enabled), ec);
            return ec ? Result<void>(std::unexpected(to_io_error("set_nodelay", ec))) : Result<void>();
        });
    }

    TaskHandle<std::expected<void, IoError>> TcpConnection::set_keepalive(bool enabled) {
        return on_strand<StreamCore, void>(core_, [enabled](StreamCore &core) -> Result<void> {
            ErrorCode ec;
            core.socket.set_option(asio::socket_base::keep_alive(enabled), ec);
            return ec ? Result<void>(std::unexpected(to_io_error("set_keepalive", ec))) : Result<void>();
        });
    }

    bool TcpConnection::is_open() const noexcept { return core_ && core_->open.load(std::memory_order_acquire); }
    void TcpConnection::close() noexcept {
        if (core_) {
            try {
                core_->request_close();
            } catch (...) {
            }
        }
    }

    std::expected<SocketAddress, IoError> TcpConnection::peer_address() const {
        if (!core_) return std::unexpected(closed_error());
        return core_->peer;
    }
    std::expected<SocketAddress, IoError> TcpConnection::local_address() const {
        if (!core_) return std::unexpected(closed_error());
        return core_->local;
    }
    bool TcpConnection::is_tls() const noexcept { return core_ && core_->tls_context != nullptr; }
    std::expected<TlsInfo, IoError> TcpConnection::tls_info() const {
        if (!core_) return std::unexpected(closed_error());
        if (!core_->tls_context) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "connection is not using TLS"});
        return core_->tls_details;
    }

    // =============================================================================================
    // TcpListener
    // =============================================================================================
    namespace {
        /// Opens and binds an acceptor on `host:port`. Empty host: dual-stack "any" (falls back to IPv4-only).
        Result<std::shared_ptr<ListenerCore>> open_listener(const std::string &host, u16 port, int backlog) {
            auto core = std::make_shared<ListenerCore>();
            ErrorCode ec;
            auto try_endpoint = [&](const tcp::endpoint &endpoint, bool dual_stack) -> bool {
                core->acceptor.open(endpoint.protocol(), ec);
                if (ec) return false;
                core->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
                if (dual_stack) core->acceptor.set_option(asio::ip::v6_only(false), ec);
                ec.clear();
                core->acceptor.bind(endpoint, ec);
                if (!ec) core->acceptor.listen(backlog > 0 ? backlog : asio::socket_base::max_listen_connections, ec);
                if (ec) {
                    ErrorCode ignored;
                    core->acceptor.close(ignored);
                    return false;
                }
                return true;
            };
            bool bound = false;
            if (host.empty()) {
                bound = try_endpoint(tcp::endpoint(asio::ip::address_v6::any(), port), true) ||
                        try_endpoint(tcp::endpoint(asio::ip::address_v4::any(), port), false);
            } else {
                const auto address = asio::ip::make_address(host, ec);
                if (!ec) {
                    bound = try_endpoint(tcp::endpoint(address, port), false);
                } else {
                    ec.clear();
                    tcp::resolver resolver(reactor());
                    const auto results = resolver.resolve(host, std::to_string(port), tcp::resolver::passive, ec);
                    if (ec) return std::unexpected(to_io_error(("resolve '" + host + "'").c_str(), ec));
                    for (const auto &entry : results) {
                        if (try_endpoint(entry.endpoint(), false)) {
                            bound = true;
                            break;
                        }
                    }
                }
            }
            if (!bound) return std::unexpected(to_io_error(("listen on " + host + ":" + std::to_string(port)).c_str(), ec));
            core->local = to_address(core->acceptor.local_endpoint(ec));
            return core;
        }
    } // namespace

    TaskHandle<std::expected<TcpListener, IoError>> TcpListener::listen(std::string host, u16 port, int backlog) {
        Promise<Result<TcpListener>> promise;
        auto opened = open_listener(host, port, backlog);
        if (!opened) {
            (void)promise.set_value(Result<TcpListener>(std::unexpected(std::move(opened.error()))));
            return promise.handle();
        }
        auto core = *opened;
        asio::post(core->strand, [core] { core->accept_loop(); });
        (void)promise.set_value(Result<TcpListener>(ListenerCore::wrap(std::move(core))));
        return promise.handle();
    }

    TaskHandle<std::expected<TcpListener, IoError>> TcpListener::listen_tls(std::string host, u16 port, TlsServerOptions tls, int backlog) {
        auto context = make_tls_server_context(tls);
        if (!context) return failed<TcpListener>(context.error());
        Promise<Result<TcpListener>> promise;
        auto opened = open_listener(host, port, backlog);
        if (!opened) {
            (void)promise.set_value(Result<TcpListener>(std::unexpected(std::move(opened.error()))));
            return promise.handle();
        }
        auto core = *opened;
        core->use_tls = true;
        core->tls_context = *context;
        core->tls_options = std::move(tls);
        asio::post(core->strand, [core] { core->accept_loop(); });
        (void)promise.set_value(Result<TcpListener>(ListenerCore::wrap(std::move(core))));
        return promise.handle();
    }

    TcpListener::~TcpListener() noexcept { close(); }
    TcpListener &TcpListener::operator=(TcpListener &&other) noexcept {
        if (this != &other) {
            close();
            core_ = std::move(other.core_);
        }
        return *this;
    }

    TaskHandle<std::expected<TcpConnection, IoError>> TcpListener::accept(CancellationToken cancellation) {
        if (!core_) return failed<TcpConnection>(closed_error("listener closed"));
        if (cancellation.is_cancelled()) return cancelled_handle<TcpConnection>();
        auto waiter = std::make_shared<AcceptWaiter>();
        auto handle = waiter->promise.handle();
        asio::post(core_->strand, [core = core_, waiter, cancellation] {
            if (!core->open.load(std::memory_order_acquire)) {
                (void)waiter->promise.set_value(Result<TcpConnection>(std::unexpected(closed_error("listener closed"))));
                return;
            }
            if (cancellation.can_be_cancelled()) {
                waiter->cancel_registration = cancellation.on_cancel([weak_core = std::weak_ptr<ListenerCore>(core), weak_waiter = std::weak_ptr<AcceptWaiter>(waiter)] {
                    auto strong_core = weak_core.lock();
                    if (!strong_core) return;
                    asio::post(strong_core->strand, [strong_core, weak_waiter] {
                        if (auto w = weak_waiter.lock(); w && !w->done) {
                            w->done = true;
                            (void)w->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                        }
                    });
                });
            }
            core->waiters.push_back(waiter);
            core->deliver();
        });
        return handle;
    }

    bool TcpListener::is_open() const noexcept { return core_ && core_->open.load(std::memory_order_acquire); }
    void TcpListener::close() noexcept {
        if (core_) {
            try {
                asio::post(core_->strand, [core = core_] { core->close_now(); });
            } catch (...) {
            }
        }
    }
    std::expected<u16, IoError> TcpListener::local_port() const {
        if (!core_) return std::unexpected(closed_error());
        return core_->local.port;
    }
    std::expected<SocketAddress, IoError> TcpListener::local_address() const {
        if (!core_) return std::unexpected(closed_error());
        return core_->local;
    }

    // =============================================================================================
    // UdpSocket
    // =============================================================================================
    UdpSocket::~UdpSocket() noexcept { close(); }
    UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept {
        if (this != &other) {
            close();
            core_ = std::move(other.core_);
        }
        return *this;
    }

    TaskHandle<std::expected<UdpSocket, IoError>> UdpSocket::bind(std::string host, u16 port, UdpOptions options) {
        auto core = std::make_shared<UdpCore>();
        ErrorCode ec;
        auto try_endpoint = [&](const udp::endpoint &endpoint, bool dual_stack) -> bool {
            core->socket.open(endpoint.protocol(), ec);
            if (ec) return false;
            if (options.reuse_address) {
                core->socket.set_option(asio::socket_base::reuse_address(true), ec);
#if defined(SO_REUSEPORT)
                if (!ec) { // several multicast/discovery listeners on one port need REUSEPORT on BSD/macOS
                    using reuse_port = asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT>;
                    core->socket.set_option(reuse_port(true), ec);
                    ec.clear();
                }
#endif
            }
            if (dual_stack) core->socket.set_option(asio::ip::v6_only(false), ec);
            ec.clear();
            if (options.broadcast) core->socket.set_option(asio::socket_base::broadcast(true), ec);
            if (options.receive_buffer > 0) core->socket.set_option(asio::socket_base::receive_buffer_size(static_cast<int>(options.receive_buffer)), ec);
            if (options.send_buffer > 0) core->socket.set_option(asio::socket_base::send_buffer_size(static_cast<int>(options.send_buffer)), ec);
            ec.clear();
            core->socket.bind(endpoint, ec);
            if (ec) {
                ErrorCode ignored;
                core->socket.close(ignored);
                return false;
            }
            return true;
        };
        bool bound = false;
        if (host.empty()) {
            bound = try_endpoint(udp::endpoint(asio::ip::address_v6::any(), port), true) ||
                    try_endpoint(udp::endpoint(asio::ip::address_v4::any(), port), false);
        } else {
            const auto address = asio::ip::make_address(host, ec);
            if (!ec) {
                bound = try_endpoint(udp::endpoint(address, port), false);
            } else {
                ec.clear();
                udp::resolver resolver(reactor());
                const auto results = resolver.resolve(host, std::to_string(port), udp::resolver::passive, ec);
                if (ec) return failed<UdpSocket>(to_io_error(("resolve '" + host + "'").c_str(), ec));
                for (const auto &entry : results) {
                    if (try_endpoint(entry.endpoint(), false)) {
                        bound = true;
                        break;
                    }
                }
            }
        }
        if (!bound) return failed<UdpSocket>(to_io_error(("bind " + host + ":" + std::to_string(port)).c_str(), ec));
        core->local = to_address(core->socket.local_endpoint(ec));
        Promise<Result<UdpSocket>> promise;
        (void)promise.set_value(Result<UdpSocket>(UdpCore::wrap(std::move(core))));
        return promise.handle();
    }

    namespace {
        /// Resolves `host:port` to a UDP endpoint on the strand, using literals directly and caching names.
        void resolve_udp(const std::shared_ptr<UdpCore> &core, std::string host, u16 port, std::function<void(Result<udp::endpoint>)> done) {
            ErrorCode ec;
            const auto address = asio::ip::make_address(host, ec);
            if (!ec) return done(udp::endpoint(address, port));
            const std::string key = host + ":" + std::to_string(port);
            if (auto it = core->resolved.find(key); it != core->resolved.end()) return done(it->second);
            core->resolver.async_resolve(host, std::to_string(port), asio::bind_executor(core->strand, [core, key, host, done = std::move(done)](const ErrorCode &error, udp::resolver::results_type results) {
                if (error) return done(std::unexpected(to_io_error(("resolve '" + host + "'").c_str(), error)));
                const udp::endpoint endpoint = results.begin()->endpoint();
                if (core->resolved.size() >= 64) core->resolved.clear();
                core->resolved[key] = endpoint;
                done(endpoint);
            }));
        }
    } // namespace

    TaskHandle<std::expected<void, IoError>> UdpSocket::connect(std::string host, u16 port) {
        Promise<Result<void>> promise;
        auto handle = promise.handle();
        if (!core_) {
            (void)promise.set_value(Result<void>(std::unexpected(closed_error())));
            return handle;
        }
        asio::post(core_->strand, [core = core_, host = std::move(host), port, promise]() mutable {
            resolve_udp(core, host, port, [core, promise](Result<udp::endpoint> endpoint) mutable {
                if (!endpoint) return (void)promise.set_value(Result<void>(std::unexpected(std::move(endpoint.error()))));
                ErrorCode ec;
                core->socket.connect(*endpoint, ec);
                (void)promise.set_value(ec ? Result<void>(std::unexpected(to_io_error("connect", ec))) : Result<void>());
            });
        });
        return handle;
    }

    TaskHandle<std::expected<std::size_t, IoError>> UdpSocket::send(std::vector<std::byte> data) {
        Promise<Result<std::size_t>> promise;
        auto handle = promise.handle();
        if (!core_) {
            (void)promise.set_value(Result<std::size_t>(std::unexpected(closed_error())));
            return handle;
        }
        asio::post(core_->strand, [core = core_, data = std::make_shared<Bytes>(std::move(data)), promise]() mutable {
            core->socket.async_send(asio::buffer(*data), asio::bind_executor(core->strand, [core, data, promise](const ErrorCode &ec, std::size_t n) mutable {
                (void)promise.set_value(ec ? Result<std::size_t>(std::unexpected(to_io_error("send", ec))) : Result<std::size_t>(n));
            }));
        });
        return handle;
    }

    TaskHandle<std::expected<std::size_t, IoError>> UdpSocket::send_to(std::string host, u16 port, std::vector<std::byte> data) {
        Promise<Result<std::size_t>> promise;
        auto handle = promise.handle();
        if (!core_) {
            (void)promise.set_value(Result<std::size_t>(std::unexpected(closed_error())));
            return handle;
        }
        asio::post(core_->strand, [core = core_, host = std::move(host), port, data = std::make_shared<Bytes>(std::move(data)), promise]() mutable {
            resolve_udp(core, host, port, [core, data, promise](Result<udp::endpoint> endpoint) mutable {
                if (!endpoint) return (void)promise.set_value(Result<std::size_t>(std::unexpected(std::move(endpoint.error()))));
                core->socket.async_send_to(asio::buffer(*data), *endpoint, asio::bind_executor(core->strand, [core, data, promise](const ErrorCode &ec, std::size_t n) mutable {
                    (void)promise.set_value(ec ? Result<std::size_t>(std::unexpected(to_io_error("send_to", ec))) : Result<std::size_t>(n));
                }));
            });
        });
        return handle;
    }

    TaskHandle<std::expected<UdpDatagram, IoError>> UdpSocket::receive_from(std::size_t max_bytes, CancellationToken cancellation) {
        if (!core_) return failed<UdpDatagram>(closed_error());
        if (max_bytes == 0 || max_bytes > 65507) return failed<UdpDatagram>(IoError{IoErrorCode::InvalidArgument, "receive_from() needs 1..65507 bytes"});
        if (cancellation.is_cancelled()) return cancelled_handle<UdpDatagram>();
        auto waiter = std::make_shared<UdpWaiter>();
        waiter->max_bytes = max_bytes;
        auto handle = waiter->promise.handle();
        asio::post(core_->strand, [core = core_, waiter, cancellation] {
            if (!core->open.load(std::memory_order_acquire)) {
                (void)waiter->promise.set_value(Result<UdpDatagram>(std::unexpected(closed_error())));
                return;
            }
            if (cancellation.can_be_cancelled()) {
                waiter->cancel_registration = cancellation.on_cancel([weak_core = std::weak_ptr<UdpCore>(core), weak_waiter = std::weak_ptr<UdpWaiter>(waiter)] {
                    auto strong_core = weak_core.lock();
                    if (!strong_core) return;
                    asio::post(strong_core->strand, [strong_core, weak_waiter] {
                        if (auto w = weak_waiter.lock(); w && !w->done) {
                            w->done = true;
                            (void)w->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                        }
                    });
                });
            }
            core->waiters.push_back(waiter);
            core->pump();
        });
        return handle;
    }

    namespace {
        TaskHandle<Result<void>> udp_option(const std::shared_ptr<UdpCore> &core, std::function<ErrorCode(UdpCore &)> apply) {
            return on_strand<UdpCore, void>(core, [apply = std::move(apply)](UdpCore &c) -> Result<void> {
                const ErrorCode ec = apply(c);
                return ec ? Result<void>(std::unexpected(to_io_error("socket option", ec))) : Result<void>();
            });
        }

        template <typename Option>
        Result<Option> multicast_option(const std::string &group, const std::string &interface_address, bool v6_scope_from_interface = false) {
            (void)v6_scope_from_interface;
            ErrorCode ec;
            const auto group_address = asio::ip::make_address(group, ec);
            if (ec || !group_address.is_multicast()) {
                return std::unexpected(IoError{IoErrorCode::InvalidArgument, "'" + group + "' is not a multicast group address"});
            }
            if (group_address.is_v4()) {
                asio::ip::address_v4 local = asio::ip::address_v4::any();
                if (!interface_address.empty()) {
                    local = asio::ip::make_address_v4(interface_address, ec);
                    if (ec) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "interface must be an IPv4 address"});
                }
                return Option(group_address.to_v4(), local);
            }
            return Option(group_address.to_v6());
        }
    } // namespace

    TaskHandle<std::expected<void, IoError>> UdpSocket::set_broadcast(bool enabled) {
        return udp_option(core_, [enabled](UdpCore &c) { ErrorCode ec; c.socket.set_option(asio::socket_base::broadcast(enabled), ec); return ec; });
    }

    TaskHandle<std::expected<void, IoError>> UdpSocket::join_multicast(std::string group, std::string interface_address) {
        auto option = multicast_option<asio::ip::multicast::join_group>(group, interface_address);
        if (!option) return failed<void>(option.error());
        return udp_option(core_, [option = *option](UdpCore &c) { ErrorCode ec; c.socket.set_option(option, ec); return ec; });
    }

    TaskHandle<std::expected<void, IoError>> UdpSocket::leave_multicast(std::string group, std::string interface_address) {
        auto option = multicast_option<asio::ip::multicast::leave_group>(group, interface_address);
        if (!option) return failed<void>(option.error());
        return udp_option(core_, [option = *option](UdpCore &c) { ErrorCode ec; c.socket.set_option(option, ec); return ec; });
    }

    TaskHandle<std::expected<void, IoError>> UdpSocket::set_multicast_loopback(bool enabled) {
        return udp_option(core_, [enabled](UdpCore &c) { ErrorCode ec; c.socket.set_option(asio::ip::multicast::enable_loopback(enabled), ec); return ec; });
    }

    TaskHandle<std::expected<void, IoError>> UdpSocket::set_multicast_ttl(unsigned hops) {
        return udp_option(core_, [hops](UdpCore &c) { ErrorCode ec; c.socket.set_option(asio::ip::multicast::hops(static_cast<int>(hops)), ec); return ec; });
    }

    bool UdpSocket::is_open() const noexcept { return core_ && core_->open.load(std::memory_order_acquire); }
    void UdpSocket::close() noexcept {
        if (core_) {
            try {
                asio::post(core_->strand, [core = core_] { core->close_now(); });
            } catch (...) {
            }
        }
    }
    std::expected<u16, IoError> UdpSocket::local_port() const {
        if (!core_) return std::unexpected(closed_error());
        return core_->local.port;
    }
    std::expected<SocketAddress, IoError> UdpSocket::local_address() const {
        if (!core_) return std::unexpected(closed_error());
        return core_->local;
    }

} // namespace SFT::Async

#else // STURDY_PLATFORM_WEB: browsers expose no raw sockets.

namespace SFT::Async {
    namespace {
        template <typename T>
        TaskHandle<std::expected<T, IoError>> unsupported() {
            Promise<std::expected<T, IoError>> promise;
            (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "raw sockets are not available on Web"}));
            return promise.handle();
        }
    } // namespace
    bool configure_network_reactor(unsigned) { return false; }
    TaskHandle<std::expected<std::vector<SocketAddress>, IoError>> resolve_host(std::string, u16) { return unsupported<std::vector<SocketAddress>>(); }
    TcpConnection::~TcpConnection() noexcept {}
    TcpConnection &TcpConnection::operator=(TcpConnection &&other) noexcept { core_ = std::move(other.core_); return *this; }
    TaskHandle<std::expected<TcpConnection, IoError>> TcpConnection::connect(std::string, u16, std::chrono::milliseconds, CancellationToken) { return unsupported<TcpConnection>(); }
    TaskHandle<std::expected<TcpConnection, IoError>> TcpConnection::connect_tls(std::string, u16, TlsClientOptions, std::chrono::milliseconds, CancellationToken) { return unsupported<TcpConnection>(); }
    TaskHandle<std::expected<std::size_t, IoError>> TcpConnection::send(std::vector<std::byte>, CancellationToken) { return unsupported<std::size_t>(); }
    TaskHandle<std::expected<void, IoError>> TcpConnection::send_all(std::vector<std::byte>, CancellationToken) { return unsupported<void>(); }
    TaskHandle<std::expected<std::vector<std::byte>, IoError>> TcpConnection::receive(std::size_t, CancellationToken) { return unsupported<std::vector<std::byte>>(); }
    TaskHandle<std::expected<std::vector<std::byte>, IoError>> TcpConnection::receive_exact(std::size_t, CancellationToken) { return unsupported<std::vector<std::byte>>(); }
    TaskHandle<std::expected<std::vector<std::byte>, IoError>> TcpConnection::receive_until(std::string, std::size_t, CancellationToken) { return unsupported<std::vector<std::byte>>(); }
    TaskHandle<std::expected<void, IoError>> TcpConnection::start_tls_client(std::string, TlsClientOptions, CancellationToken) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> TcpConnection::start_tls_server(TlsServerOptions) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> TcpConnection::shutdown_write() { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> TcpConnection::set_nodelay(bool) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> TcpConnection::set_keepalive(bool) { return unsupported<void>(); }
    bool TcpConnection::is_open() const noexcept { return false; }
    void TcpConnection::close() noexcept {}
    std::expected<SocketAddress, IoError> TcpConnection::peer_address() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    std::expected<SocketAddress, IoError> TcpConnection::local_address() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    bool TcpConnection::is_tls() const noexcept { return false; }
    std::expected<TlsInfo, IoError> TcpConnection::tls_info() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    TcpListener::~TcpListener() noexcept {}
    TcpListener &TcpListener::operator=(TcpListener &&other) noexcept { core_ = std::move(other.core_); return *this; }
    TaskHandle<std::expected<TcpListener, IoError>> TcpListener::listen(std::string, u16, int) { return unsupported<TcpListener>(); }
    TaskHandle<std::expected<TcpListener, IoError>> TcpListener::listen_tls(std::string, u16, TlsServerOptions, int) { return unsupported<TcpListener>(); }
    TaskHandle<std::expected<TcpConnection, IoError>> TcpListener::accept(CancellationToken) { return unsupported<TcpConnection>(); }
    bool TcpListener::is_open() const noexcept { return false; }
    void TcpListener::close() noexcept {}
    std::expected<u16, IoError> TcpListener::local_port() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    std::expected<SocketAddress, IoError> TcpListener::local_address() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    UdpSocket::~UdpSocket() noexcept {}
    UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept { core_ = std::move(other.core_); return *this; }
    TaskHandle<std::expected<UdpSocket, IoError>> UdpSocket::bind(std::string, u16, UdpOptions) { return unsupported<UdpSocket>(); }
    TaskHandle<std::expected<void, IoError>> UdpSocket::connect(std::string, u16) { return unsupported<void>(); }
    TaskHandle<std::expected<std::size_t, IoError>> UdpSocket::send(std::vector<std::byte>) { return unsupported<std::size_t>(); }
    TaskHandle<std::expected<std::size_t, IoError>> UdpSocket::send_to(std::string, u16, std::vector<std::byte>) { return unsupported<std::size_t>(); }
    TaskHandle<std::expected<UdpDatagram, IoError>> UdpSocket::receive_from(std::size_t, CancellationToken) { return unsupported<UdpDatagram>(); }
    TaskHandle<std::expected<void, IoError>> UdpSocket::set_broadcast(bool) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> UdpSocket::join_multicast(std::string, std::string) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> UdpSocket::leave_multicast(std::string, std::string) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> UdpSocket::set_multicast_loopback(bool) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> UdpSocket::set_multicast_ttl(unsigned) { return unsupported<void>(); }
    bool UdpSocket::is_open() const noexcept { return false; }
    void UdpSocket::close() noexcept {}
    std::expected<u16, IoError> UdpSocket::local_port() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
    std::expected<SocketAddress, IoError> UdpSocket::local_address() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "unsupported on Web"}); }
} // namespace SFT::Async

#endif
