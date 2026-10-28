#include <Async/Relay.hpp>

#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Timer.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <optional>
#include <string_view>

namespace SFT::Async {

    std::expected<std::string, IoError> random_relay_token() {
        unsigned char bytes[16];
        if (RAND_bytes(bytes, sizeof bytes) != 1) return std::unexpected(IoError{IoErrorCode::Unknown, "OpenSSL random generator failed"});
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        for (unsigned char b : bytes) {
            out += digits[b >> 4];
            out += digits[b & 15];
        }
        return out;
    }

    namespace Detail {

        namespace {
        using Bytes = std::vector<std::byte>;

        /// A token that fires when `duration` passes or `outer` is cancelled; the timer is dropped with the object.
        class Deadline {
          public:
            Deadline(std::chrono::milliseconds duration, const CancellationToken &outer = {})
                : source_(std::make_shared<CancellationSource>()), timer_stop_(std::make_shared<CancellationSource>()) {
                auto timer = sleep_for(duration, timer_stop_->token());
                timer.on_done([timer, source = source_]() mutable {
                    try {
                        timer.wait();
                        (void)source->cancel();
                    } catch (...) {
                    }
                });
                if (outer.can_be_cancelled()) outer_ = outer.on_cancel([source = source_] { (void)source->cancel(); });
            }
            ~Deadline() { (void)timer_stop_->cancel(); }
            Deadline(const Deadline &) = delete;
            Deadline &operator=(const Deadline &) = delete;
            [[nodiscard]] CancellationToken token() const { return source_->token(); }

          private:
            std::shared_ptr<CancellationSource> source_;
            std::shared_ptr<CancellationSource> timer_stop_;
            std::optional<CancellationRegistration> outer_;
        };

        bool valid_token(const std::string &token, std::size_t max_bytes) {
            if (token.empty() || token.size() > max_bytes) return false;
            return std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '~' || c == '-'; });
        }

        Bytes line(std::string_view text) {
            Bytes out(text.size());
            std::memcpy(out.data(), text.data(), text.size());
            return out;
        }

        struct Waiting {
            RelayRole role = RelayRole::Dial;
            std::shared_ptr<TcpConnection> connection;
            std::shared_ptr<CancellationSource> wake; ///< cancelled when the entry is taken (paired) so its watcher stands down
            std::string address;
            std::uint64_t serial = 0;
        };

        struct WaitTable {
            std::map<std::string, Waiting> by_token;
            std::map<std::string, std::size_t> per_address;
            std::uint64_t next_serial = 1;
        };

        struct Session {
            std::shared_ptr<TcpConnection> a;
            std::shared_ptr<TcpConnection> b;
            CancellationSource cut;
            std::atomic<std::uint64_t> bytes{0};
            std::atomic<std::int64_t> last_activity_ms{0};
        };

        std::int64_t relay_now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

        } // namespace

        struct RelayState : std::enable_shared_from_this<RelayState> {
            RelayServerOptions options;
            TcpListener listener;
            CancellationSource stop;
            Mutex<WaitTable> table;
            std::atomic<bool> stopped{false};
            std::atomic<std::uint64_t> sessions_started{0};
            std::atomic<std::uint64_t> sessions_active{0};
            std::atomic<std::uint64_t> waiting{0};
            std::atomic<std::uint64_t> bytes_relayed{0};
            std::atomic<std::uint64_t> rejected{0};
            std::atomic<std::uint64_t> timed_out{0};
        };

        namespace {
        void forget_address(WaitTable &table, const std::string &address) {
            if (auto it = table.per_address.find(address); it != table.per_address.end() && --it->second == 0) table.per_address.erase(it);
        }

        TaskHandle<void> reject(std::shared_ptr<RelayState> state, std::shared_ptr<TcpConnection> connection, std::string reason) {
            ++state->rejected;
            (void)(co_await connection->send_all(line("ERR " + reason + "\n")));
            connection->close();
        }

        TaskHandle<void> pump(std::shared_ptr<RelayState> state, std::shared_ptr<Session> session, std::shared_ptr<TcpConnection> from, std::shared_ptr<TcpConnection> to) {
            try {
                for (;;) {
                    auto data = co_await from->receive(64 * 1024, session->cut.token());
                    if (!data) break;
                    if (data->empty()) {
                        (void)(co_await to->shutdown_write()); // pass the half-close on; the other direction keeps flowing
                        co_return;
                    }
                    const std::uint64_t size = data->size();
                    const std::uint64_t total = session->bytes.fetch_add(size) + size;
                    state->bytes_relayed += size;
                    session->last_activity_ms = relay_now_ms();
                    if (state->options.max_session_bytes != 0 && total > state->options.max_session_bytes) break;
                    auto sent = co_await to->send_all(std::move(*data), session->cut.token());
                    if (!sent) break;
                    session->last_activity_ms = relay_now_ms();
                }
            } catch (...) {
            }
            (void)session->cut.cancel(); // an error or limit on one side ends the whole session
        }

        TaskHandle<void> watch_idle(std::shared_ptr<RelayState> state, std::shared_ptr<Session> session, std::shared_ptr<CancellationSource> done) {
            const auto step = (std::max)(std::chrono::milliseconds(50), state->options.idle_timeout / 4);
            try {
                for (;;) {
                    co_await sleep_for(step, done->token());
                    if (relay_now_ms() - session->last_activity_ms.load() > state->options.idle_timeout.count()) {
                        (void)session->cut.cancel();
                        co_return;
                    }
                }
            } catch (...) {
            }
        }

        TaskHandle<void> run_session(std::shared_ptr<RelayState> state, std::shared_ptr<TcpConnection> a, std::shared_ptr<TcpConnection> b) {
            auto session = std::make_shared<Session>();
            session->a = std::move(a);
            session->b = std::move(b);
            session->last_activity_ms = relay_now_ms();
            ++state->sessions_started;
            ++state->sessions_active;

            auto stop_registration = state->stop.token().on_cancel([session] { (void)session->cut.cancel(); });
            auto finished = std::make_shared<CancellationSource>();
            (void)watch_idle(state, session, finished);

            auto forward = pump(state, session, session->a, session->b);
            auto backward = pump(state, session, session->b, session->a);
            try {
                co_await forward;
                co_await backward;
            } catch (...) {
            }
            (void)finished->cancel();
            session->a->close();
            session->b->close();
            --state->sessions_active;
        }

        /// Reads the hello line, then either parks the connection until its partner shows up or joins the two.
        TaskHandle<void> handle_connection(std::shared_ptr<RelayState> state, std::shared_ptr<TcpConnection> connection) {
            const std::string address = connection->peer_address().value_or(SocketAddress{}).host;

            std::string text;
            {
                Deadline deadline(std::chrono::milliseconds(10'000), state->stop.token());
                try {
                    auto hello = co_await connection->receive_until("\n", 256, deadline.token());
                    if (!hello) {
                        connection->close();
                        co_return;
                    }
                    text.assign(reinterpret_cast<const char *>(hello->data()), hello->size());
                } catch (...) {
                    connection->close();
                    co_return;
                }
            }
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            // "SRELAY 1 <dial|accept> <token>"
            std::string magic, version, role_text, token, extra;
            {
                std::size_t position = 0;
                auto next = [&](std::string &out) {
                    const auto end = text.find(' ', position);
                    out = text.substr(position, end == std::string::npos ? std::string::npos : end - position);
                    position = end == std::string::npos ? text.size() : end + 1;
                };
                next(magic);
                next(version);
                next(role_text);
                next(token);
                next(extra);
            }
            const bool dial = role_text == "dial";
            if (magic != "SRELAY" || version != "1" || (!dial && role_text != "accept") || !extra.empty() || !valid_token(token, state->options.max_token_bytes)) {
                co_await reject(state, connection, "bad request");
                co_return;
            }
            const RelayRole role = dial ? RelayRole::Dial : RelayRole::Accept;
            if (state->options.authorize && !state->options.authorize(token, address)) {
                co_await reject(state, connection, "not authorized");
                co_return;
            }
            if (state->stopped.load()) {
                connection->close();
                co_return;
            }

            // Pair or park.
            std::optional<Waiting> partner;
            std::shared_ptr<CancellationSource> wake;
            std::uint64_t serial = 0;
            std::optional<std::string> refusal;
            {
                auto guard = state->table.lock();
                if (auto it = guard->by_token.find(token); it != guard->by_token.end()) {
                    if (it->second.role == role) {
                        refusal = "token busy";
                    } else if (state->sessions_active.load() >= state->options.max_sessions) {
                        refusal = "relay is full";
                    } else {
                        partner = std::move(it->second);
                        forget_address(*guard, partner->address);
                        guard->by_token.erase(it);
                        --state->waiting;
                    }
                } else if (guard->by_token.size() >= state->options.max_waiting) {
                    refusal = "relay is full";
                } else if (guard->per_address[address] >= state->options.max_waiting_per_address) {
                    refusal = "too many waiting connections from your address";
                } else {
                    wake = std::make_shared<CancellationSource>();
                    serial = guard->next_serial++;
                    ++guard->per_address[address];
                    guard->by_token.emplace(token, Waiting{role, connection, wake, address, serial});
                    ++state->waiting;
                }
            }
            if (refusal) {
                co_await reject(state, connection, *refusal);
                co_return;
            }

            if (partner) {
                (void)partner->wake->cancel(); // the partner's watcher stands down
                auto ok_to_partner = co_await partner->connection->send_all(line("OK\n"));
                auto ok_to_us = co_await connection->send_all(line("OK\n"));
                if (!ok_to_partner || !ok_to_us) {
                    // One side vanished while waiting: tell the survivor, who can simply retry.
                    if (ok_to_partner) (void)(co_await partner->connection->send_all(line("ERR partner left\n")));
                    if (ok_to_us) (void)(co_await connection->send_all(line("ERR partner left\n")));
                    partner->connection->close();
                    connection->close();
                    co_return;
                }
                co_await run_session(state, partner->connection, connection);
                co_return;
            }

            // Parked: watch for the peer leaving, the timeout, and shutdown. Pairing cancels `wake`.
            auto stop_registration = state->stop.token().on_cancel([wake] { (void)wake->cancel(); });
            Deadline patience(state->options.pair_timeout);
            const CancellationToken timeout_token = patience.token();
            auto timeout_registration = timeout_token.on_cancel([wake] { (void)wake->cancel(); });
            try {
                auto activity = co_await connection->receive(1, wake->token());
                (void)activity; // a peer that talks before being paired, or hangs up, is dropped below
            } catch (...) {
            }
            bool still_ours = false;
            {
                auto guard = state->table.lock();
                if (auto it = guard->by_token.find(token); it != guard->by_token.end() && it->second.serial == serial) {
                    forget_address(*guard, address);
                    guard->by_token.erase(it);
                    --state->waiting;
                    still_ours = true;
                }
            }
            if (!still_ours) co_return; // paired: the partner's coroutine runs the session
            if (timeout_token.is_cancelled() && !state->stopped.load()) {
                ++state->timed_out;
                (void)(co_await connection->send_all(line("ERR no partner arrived\n")));
            }
            connection->close();
        }

        TaskHandle<void> accept_loop(std::shared_ptr<RelayState> state) {
            try {
                for (;;) {
                    auto accepted = co_await state->listener.accept(state->stop.token());
                    if (state->stopped.load()) break;
                    if (!accepted) {
                        if (accepted.error().code == IoErrorCode::Closed) break;
                        continue;
                    }
                    (void)handle_connection(state, std::make_shared<TcpConnection>(std::move(*accepted)));
                }
            } catch (...) {
            }
        }

        } // namespace
    } // namespace Detail

    RelayServer::~RelayServer() noexcept { stop(); }
    RelayServer &RelayServer::operator=(RelayServer &&other) noexcept {
        if (this != &other) {
            stop();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    TaskHandle<std::expected<RelayServer, IoError>> RelayServer::start(RelayServerOptions options) {
        auto fail = [](IoError error) {
            Promise<std::expected<RelayServer, IoError>> promise;
            (void)promise.set_value(std::unexpected(std::move(error)));
            return promise.handle();
        };
        if (options.max_sessions == 0 || options.max_waiting == 0 || options.max_waiting_per_address == 0 || options.pair_timeout.count() <= 0 ||
            options.idle_timeout.count() <= 0 || options.max_token_bytes == 0 || options.max_token_bytes > 256) {
            return fail(IoError{IoErrorCode::InvalidArgument, "invalid relay limits"});
        }
        auto state = std::make_shared<Detail::RelayState>();
        state->options = std::move(options);
        auto listener = state->options.tls ? TcpListener::listen_tls(state->options.bind_address, state->options.port, *state->options.tls).wait()
                                           : TcpListener::listen(state->options.bind_address, state->options.port).wait();
        if (!listener) return fail(listener.error());
        state->listener = std::move(*listener);
        RelayServer server;
        server.state_ = state;
        (void)Detail::accept_loop(state);
        Promise<std::expected<RelayServer, IoError>> promise;
        (void)promise.set_value(std::expected<RelayServer, IoError>(std::move(server)));
        return promise.handle();
    }

    std::expected<std::uint16_t, IoError> RelayServer::local_port() const {
        if (!state_) return std::unexpected(IoError{IoErrorCode::Closed, "relay not started"});
        return state_->listener.local_port();
    }

    RelayMetrics RelayServer::metrics() const noexcept {
        RelayMetrics m;
        if (!state_) return m;
        m.sessions_started = state_->sessions_started.load();
        m.sessions_active = state_->sessions_active.load();
        m.waiting = state_->waiting.load();
        m.bytes_relayed = state_->bytes_relayed.load();
        m.rejected = state_->rejected.load();
        m.timed_out = state_->timed_out.load();
        return m;
    }

    void RelayServer::stop() noexcept {
        if (!state_) return;
        if (state_->stopped.exchange(true)) return;
        (void)state_->stop.cancel();
        state_->listener.close();
    }

    TaskHandle<std::expected<TcpConnection, IoError>> relay_connect(std::string relay_host, std::uint16_t relay_port, std::string token, RelayRole role,
                                                                     std::optional<TlsClientOptions> relay_tls, std::chrono::milliseconds timeout,
                                                                     CancellationToken cancellation) {
        using Result = std::expected<TcpConnection, IoError>;
        if (!Detail::valid_token(token, 256)) co_return Result(std::unexpected(IoError{IoErrorCode::InvalidArgument, "relay tokens are 1-256 characters of A-Z a-z 0-9 . _ ~ -"}));
        Detail::Deadline deadline(timeout, cancellation);
        try {
            Result connected = std::unexpected(IoError{IoErrorCode::Unknown, "unreachable"});
            if (relay_tls) connected = co_await TcpConnection::connect_tls(relay_host, relay_port, *relay_tls, timeout, deadline.token());
            else connected = co_await TcpConnection::connect(relay_host, relay_port, timeout, deadline.token());
            if (!connected) co_return Result(std::unexpected(connected.error()));
            TcpConnection connection = std::move(*connected);
            const std::string hello = std::string("SRELAY 1 ") + (role == RelayRole::Dial ? "dial " : "accept ") + token + "\n";
            auto sent = co_await connection.send_all(Detail::line(hello), deadline.token());
            if (!sent) co_return Result(std::unexpected(sent.error()));
            auto answer = co_await connection.receive_until("\n", 128, deadline.token());
            if (!answer) co_return Result(std::unexpected(answer.error()));
            std::string text(reinterpret_cast<const char *>(answer->data()), answer->size());
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            if (text != "OK") {
                const bool refused = text.rfind("ERR ", 0) == 0;
                co_return Result(std::unexpected(IoError{refused ? IoErrorCode::ConnectionRefused : IoErrorCode::ProtocolError,
                                                         "relay: " + (refused ? text.substr(4) : std::string("unexpected answer"))}));
            }
            co_return Result(std::move(connection));
        } catch (const IoOperationCancelled &) {
            if (cancellation.is_cancelled()) throw;
            co_return Result(std::unexpected(IoError{IoErrorCode::TimedOut, "relay: no partner arrived in time"}));
        }
    }

} // namespace SFT::Async

#else

namespace SFT::Async {
    std::expected<std::string, IoError> random_relay_token() { return std::unexpected(IoError{IoErrorCode::Unsupported, "relay is not available on Web"}); }
    RelayServer::~RelayServer() noexcept {}
    RelayServer &RelayServer::operator=(RelayServer &&) noexcept = default;
    TaskHandle<std::expected<RelayServer, IoError>> RelayServer::start(RelayServerOptions) {
        Promise<std::expected<RelayServer, IoError>> promise;
        (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "relay is not available on Web"}));
        return promise.handle();
    }
    std::expected<std::uint16_t, IoError> RelayServer::local_port() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "relay is not available on Web"}); }
    RelayMetrics RelayServer::metrics() const noexcept { return {}; }
    void RelayServer::stop() noexcept {}
    TaskHandle<std::expected<TcpConnection, IoError>> relay_connect(std::string, std::uint16_t, std::string, RelayRole, std::optional<TlsClientOptions>,
                                                                     std::chrono::milliseconds, CancellationToken) {
        Promise<std::expected<TcpConnection, IoError>> promise;
        (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "relay is not available on Web"}));
        return promise.handle();
    }
} // namespace SFT::Async

#endif
