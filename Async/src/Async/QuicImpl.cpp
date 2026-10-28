#include <Async/Quic.hpp>

#include <Async/Promise.hpp>

#if defined(STURDY_ASYNC_QUIC) && !defined(STURDY_PLATFORM_WEB)

#include <Async/Mutex.hpp>
#include <Async/NetCore.hpp>
#include <Async/QuicEngine.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <optional>
#include <unordered_map>

namespace SFT::Async {

    bool quic_available() noexcept { return true; }

    namespace Detail {

        using Bytes = std::vector<std::byte>;

        // =================================================================================================
        // Raw stream layer
        // =================================================================================================
        struct RecvWaiter {
            std::size_t max = 0;
            Promise<Result<Bytes>> promise;
            CancellationRegistration registration;
            bool done = false;
        };

        struct PendingSend {
            Bytes data;
            Promise<Result<void>> promise;
            CancellationRegistration registration;
            bool done = false;
        };

        struct QuicStreamState {
            std::int64_t id = -1;
            bool bidirectional = true;

            // receive side (strand only)
            std::deque<Bytes> chunks;
            std::size_t buffered = 0;
            bool fin = false;
            std::optional<std::uint64_t> reset_code;
            bool read_stopped = false;
            std::deque<std::shared_ptr<RecvWaiter>> readers;

            // send side
            struct Chunk {
                std::shared_ptr<Bytes> data;
                std::uint64_t start = 0;
            };
            std::deque<Chunk> out;
            std::uint64_t queued = 0;
            std::uint64_t sent = 0;
            std::uint64_t acked = 0;
            bool fin_requested = false;
            bool fin_written = false;
            bool write_shut = false;
            bool blocked = false;
            bool in_active = false;
            bool closed = false;
            std::deque<std::shared_ptr<PendingSend>> pending;
        };

        struct StreamOpenWaiter {
            bool bidirectional = true;
            std::function<void(Result<std::shared_ptr<QuicStreamState>>)> done;
            CancellationRegistration registration;
            bool finished = false;
        };

        struct QuicAcceptWaiter {
            std::function<void(Result<std::shared_ptr<QuicStreamState>>)> done;
            CancellationRegistration registration;
            bool finished = false;
        };

        struct DatagramWaiter {
            Promise<Result<Bytes>> promise;
            CancellationRegistration registration;
            bool done = false;
        };

        struct OutDatagram {
            std::vector<std::uint8_t> data;
            std::uint64_t id = 0;
            std::chrono::steady_clock::time_point queued;
        };

        struct DatagramOut {
            std::deque<OutDatagram> queue;
            std::uint64_t next_id = 1;
        };

        struct CloseState {
            bool closed = false;
            QuicCloseInfo info;
            std::vector<Promise<QuicCloseInfo>> waiters;
        };

        IoError close_error(const QuicCloseInfo &info) {
            if (info.timed_out) return IoError{IoErrorCode::TimedOut, "QUIC: " + info.reason};
            if (!info.application && (info.reason.find("TLS") != std::string::npos || info.reason.find("certificate") != std::string::npos)) {
                return IoError{IoErrorCode::TlsFailure, "QUIC: " + info.reason};
            }
            std::string text = "QUIC connection closed";
            if (info.from_peer) text += " by the peer";
            text += " (code " + std::to_string(info.code) + ")";
            if (!info.reason.empty()) text += ": " + info.reason;
            return IoError{info.from_peer ? IoErrorCode::ConnectionReset : IoErrorCode::Closed, std::move(text)};
        }

        class RawApp final : public QuicApp {
          public:
            explicit RawApp(QuicConn &conn) : conn_(&conn) {}

            // ---- configuration set before the connection starts ----
            std::function<void(Result<void>)> on_handshake; ///< client connect(): called once, then cleared
            std::function<void()> on_ready;                 ///< server: handshake finished
            /// Client with a certificate: only report success once the server confirmed it accepted that certificate
            /// (TLS 1.3 lets the client finish before the server has judged the client certificate).
            bool wait_for_confirmation = false;

            // ---- shared with the public handle (thread-safe) ----
            Mutex<DatagramOut> datagrams_out;
            Mutex<CloseState> close_state;

            // ================================ QuicApp ================================
            void handshake_completed() override {
                handshake_done_ = true;
                if (!wait_for_confirmation) fire_handshake({});
            }

            void handshake_confirmed() override { fire_handshake({}); }

            bool recv_stream_data(std::int64_t id, bool fin, const std::uint8_t *data, std::size_t size) override {
                auto st = stream_for(id);
                if (!st) return true;
                if (size > 0) {
                    Bytes chunk(size);
                    std::memcpy(chunk.data(), data, size);
                    st->chunks.push_back(std::move(chunk));
                    st->buffered += size;
                }
                if (fin) st->fin = true;
                serve_readers(*st);
                return true;
            }

            bool acked_stream_data(std::int64_t id, std::uint64_t size) override {
                auto it = streams_.find(id);
                if (it == streams_.end()) return true;
                auto &st = *it->second;
                st.acked += size;
                while (!st.out.empty() && st.out.front().start + st.out.front().data->size() <= st.acked) st.out.pop_front();
                drain_pending(st);
                return true;
            }

            void stream_open(std::int64_t id) override {
                if (ngtcp2_conn_is_local_stream(conn_->raw(), id) != 0) return;
                auto st = stream_for(id);
                if (!st) return;
                deliver_accepted(st);
            }

            void stream_close(std::int64_t id, std::uint64_t app_error_code) override {
                auto it = streams_.find(id);
                if (it == streams_.end()) return;
                auto st = it->second;
                streams_.erase(it);
                st->closed = true;
                st->out.clear();
                if (!st->fin && !st->reset_code) st->reset_code = app_error_code;
                fail_pending(*st, IoError{IoErrorCode::Closed, "stream closed"});
                serve_readers(*st);
            }

            void stream_reset(std::int64_t id, std::uint64_t code) override {
                auto it = streams_.find(id);
                if (it == streams_.end()) return;
                auto &st = *it->second;
                st.reset_code = code;
                // Discarded data frees its connection-level credit.
                if (st.buffered > 0) ngtcp2_conn_extend_max_offset(conn_->raw(), st.buffered);
                st.chunks.clear();
                st.buffered = 0;
                serve_readers(st);
                conn_->request_flush();
            }

            void stream_stop_sending(std::int64_t id, std::uint64_t code) override {
                auto it = streams_.find(id);
                if (it == streams_.end()) return;
                auto &st = *it->second;
                st.write_shut = true;
                fail_pending(st, IoError{IoErrorCode::ConnectionReset, "the peer asked us to stop sending (code " + std::to_string(code) + ")"});
                ngtcp2_conn_shutdown_stream_write(conn_->raw(), 0, id, code);
                conn_->request_flush();
            }

            void extend_max_local_streams(bool bidirectional) override {
                // FIFO: each waiter whose kind matches gets a stream while the peer's limit allows.
                for (auto it = open_waiters_.begin(); it != open_waiters_.end();) {
                    auto waiter = *it;
                    if (waiter->finished) {
                        it = open_waiters_.erase(it);
                        continue;
                    }
                    if (waiter->bidirectional != bidirectional) {
                        ++it;
                        continue;
                    }
                    std::int64_t id = -1;
                    const int rv = bidirectional ? ngtcp2_conn_open_bidi_stream(conn_->raw(), &id, nullptr) : ngtcp2_conn_open_uni_stream(conn_->raw(), &id, nullptr);
                    if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) break;
                    waiter->finished = true;
                    waiter->registration.reset();
                    it = open_waiters_.erase(it);
                    if (rv != 0) {
                        waiter->done(std::unexpected(IoError{IoErrorCode::Unknown, std::string("cannot open a stream: ") + ngtcp2_strerror(rv)}));
                    } else {
                        waiter->done(stream_for(id, bidirectional));
                    }
                }
            }

            void extend_max_stream_data(std::int64_t id) override {
                auto it = streams_.find(id);
                if (it == streams_.end()) return;
                it->second->blocked = false;
                activate(*it->second);
            }

            void recv_datagram(const std::uint8_t *data, std::size_t size) override {
                Bytes copy(size);
                std::memcpy(copy.data(), data, size);
                // Datagrams are lossy by contract: when nobody keeps up, the oldest are dropped.
                while (!datagram_waiters_.empty()) {
                    auto waiter = datagram_waiters_.front();
                    datagram_waiters_.pop_front();
                    if (waiter->done) continue;
                    waiter->done = true;
                    waiter->registration.reset();
                    (void)waiter->promise.set_value(Result<Bytes>(std::move(copy)));
                    return;
                }
                datagrams_in_.push_back(std::move(copy));
                while (datagrams_in_.size() > conn_->limits().datagram_receive_queue) datagrams_in_.pop_front();
            }

            void closed(const QuicCloseInfo &info) override {
                closed_ = true;
                const IoError error = close_error(info);
                if (on_handshake) {
                    auto callback = std::move(on_handshake);
                    on_handshake = nullptr;
                    callback(std::unexpected(error));
                }
                // Everything waiting fails; reads of already-buffered data still succeed until drained.
                auto all = std::move(streams_);
                streams_.clear();
                for (auto &entry : all) {
                    auto &st = *entry.second;
                    st.closed = true;
                    st.out.clear();
                    if (!st.fin && !st.reset_code) st.reset_code = info.code;
                    fail_pending(st, error);
                    serve_readers(st);
                }
                for (auto &waiter : open_waiters_) {
                    if (waiter->finished) continue;
                    waiter->finished = true;
                    waiter->done(std::unexpected(error));
                }
                open_waiters_.clear();
                for (auto &waiter : accept_waiters_) {
                    if (waiter->finished) continue;
                    waiter->finished = true;
                    waiter->done(std::unexpected(error));
                }
                accept_waiters_.clear();
                for (auto &waiter : datagram_waiters_) {
                    if (waiter->done) continue;
                    waiter->done = true;
                    (void)waiter->promise.set_value(Result<Bytes>(std::unexpected(error)));
                }
                datagram_waiters_.clear();
                std::vector<Promise<QuicCloseInfo>> waiters;
                {
                    auto guard = close_state.lock();
                    guard->closed = true;
                    guard->info = info;
                    waiters.swap(guard->waiters);
                }
                for (auto &waiter : waiters) (void)waiter.set_value(QuicCloseInfo(info));
            }

            bool next_stream_write(StreamWrite &out) override {
                for (std::size_t attempts = active_.size(); attempts > 0; --attempts) {
                    auto st = active_.front();
                    active_.pop_front();
                    st->in_active = false;
                    if (st->closed || st->write_shut || st->blocked) continue;
                    const std::uint64_t available = st->queued - st->sent;
                    const bool fin_ready = st->fin_requested && !st->fin_written;
                    if (available == 0 && !fin_ready) continue;

                    out.stream_id = st->id;
                    out.veccnt = 0;
                    out.offered = 0;
                    std::uint64_t position = st->sent;
                    for (const auto &chunk : st->out) {
                        const std::uint64_t end = chunk.start + chunk.data->size();
                        if (end <= position) continue;
                        if (out.veccnt == std::size(out.vec)) break;
                        const std::uint64_t skip = position > chunk.start ? position - chunk.start : 0;
                        out.vec[out.veccnt].base = reinterpret_cast<std::uint8_t *>(chunk.data->data()) + skip;
                        out.vec[out.veccnt].len = static_cast<std::size_t>(chunk.data->size() - skip);
                        out.offered += out.vec[out.veccnt].len;
                        position += out.vec[out.veccnt].len;
                        ++out.veccnt;
                    }
                    out.fin = fin_ready && out.offered == available;
                    st->in_active = true;
                    active_.push_back(st); // round robin: next time, someone else goes first
                    return true;
                }
                return false;
            }

            void stream_write_done(const StreamWrite &out, std::size_t bytes) override {
                auto it = streams_.find(out.stream_id);
                if (it == streams_.end()) return;
                auto &st = *it->second;
                st.sent += bytes;
                if (out.fin && bytes == out.offered) st.fin_written = true;
            }

            void stream_write_blocked(std::int64_t id) override {
                if (auto it = streams_.find(id); it != streams_.end()) it->second->blocked = true;
            }

            void stream_write_shut(std::int64_t id) override {
                if (auto it = streams_.find(id); it != streams_.end()) {
                    it->second->write_shut = true;
                    fail_pending(*it->second, IoError{IoErrorCode::ConnectionReset, "the stream's sending side is closed"});
                }
            }

            bool next_datagram(std::vector<std::uint8_t> &out, std::uint64_t &id) override {
                auto guard = datagrams_out.lock();
                const auto now = std::chrono::steady_clock::now();
                while (!guard->queue.empty() && now - guard->queue.front().queued > std::chrono::seconds(1)) guard->queue.pop_front(); // stale: useless now
                if (guard->queue.empty()) return false;
                out = guard->queue.front().data;
                id = guard->queue.front().id;
                return true;
            }

            void datagram_accepted(std::uint64_t id) override {
                auto guard = datagrams_out.lock();
                if (!guard->queue.empty() && guard->queue.front().id == id) guard->queue.pop_front();
            }

            // ================================ operations (strand only) ================================
            void do_send(const std::shared_ptr<QuicStreamState> &st, Bytes data, Promise<Result<void>> promise, const CancellationToken &token) {
                if (closed_ || st->closed || st->write_shut || st->fin_requested) {
                    (void)promise.set_value(Result<void>(std::unexpected(IoError{IoErrorCode::Closed, "stream is not writable"})));
                    return;
                }
                if (data.empty()) {
                    (void)promise.set_value(Result<void>());
                    return;
                }
                auto pending = std::make_shared<PendingSend>();
                pending->data = std::move(data);
                pending->promise = promise;
                if (token.can_be_cancelled()) {
                    pending->registration = token.on_cancel([weak = std::weak_ptr<PendingSend>(pending), conn = conn_->shared_from_this()] {
                        asio::post(conn->strand(), [weak] {
                            if (auto p = weak.lock(); p && !p->done) {
                                p->done = true;
                                (void)p->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                            }
                        });
                    });
                }
                st->pending.push_back(std::move(pending));
                drain_pending(*st);
            }

            void do_finish(const std::shared_ptr<QuicStreamState> &st, Promise<Result<void>> promise) {
                if (closed_ || st->closed || st->write_shut) {
                    (void)promise.set_value(Result<void>(std::unexpected(IoError{IoErrorCode::Closed, "stream is not writable"})));
                    return;
                }
                st->fin_requested = true;
                activate(*st);
                conn_->request_flush();
                (void)promise.set_value(Result<void>());
            }

            void do_receive(const std::shared_ptr<QuicStreamState> &st, std::size_t max, Promise<Result<Bytes>> promise, const CancellationToken &token) {
                auto waiter = std::make_shared<RecvWaiter>();
                waiter->max = (std::max<std::size_t>)(max, 1);
                waiter->promise = promise;
                if (token.can_be_cancelled()) {
                    waiter->registration = token.on_cancel([weak = std::weak_ptr<RecvWaiter>(waiter), conn = conn_->shared_from_this()] {
                        asio::post(conn->strand(), [weak] {
                            if (auto w = weak.lock(); w && !w->done) {
                                w->done = true;
                                (void)w->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                            }
                        });
                    });
                }
                st->readers.push_back(std::move(waiter));
                serve_readers(*st);
            }

            void do_reset(const std::shared_ptr<QuicStreamState> &st, std::uint64_t code) {
                if (closed_ || st->closed) return;
                st->write_shut = true;
                fail_pending(*st, IoError{IoErrorCode::Closed, "stream was reset locally"});
                ngtcp2_conn_shutdown_stream_write(conn_->raw(), 0, st->id, code);
                conn_->request_flush();
            }

            void do_stop_sending(const std::shared_ptr<QuicStreamState> &st, std::uint64_t code) {
                if (closed_ || st->closed) return;
                st->read_stopped = true;
                if (st->buffered > 0) ngtcp2_conn_extend_max_offset(conn_->raw(), st->buffered);
                st->chunks.clear();
                st->buffered = 0;
                ngtcp2_conn_shutdown_stream_read(conn_->raw(), 0, st->id, code);
                serve_readers(*st);
                conn_->request_flush();
            }

            void do_open(bool bidirectional, std::function<void(Result<std::shared_ptr<QuicStreamState>>)> done, const CancellationToken &token) {
                if (closed_) return done(std::unexpected(IoError{IoErrorCode::Closed, "connection is closed"}));
                std::int64_t id = -1;
                const int rv = bidirectional ? ngtcp2_conn_open_bidi_stream(conn_->raw(), &id, nullptr) : ngtcp2_conn_open_uni_stream(conn_->raw(), &id, nullptr);
                if (rv == 0) return done(stream_for(id, bidirectional));
                if (rv != NGTCP2_ERR_STREAM_ID_BLOCKED) {
                    return done(std::unexpected(IoError{IoErrorCode::Unknown, std::string("cannot open a stream: ") + ngtcp2_strerror(rv)}));
                }
                auto waiter = std::make_shared<StreamOpenWaiter>();
                waiter->bidirectional = bidirectional;
                waiter->done = std::move(done);
                if (token.can_be_cancelled()) {
                    waiter->registration = token.on_cancel([weak = std::weak_ptr<StreamOpenWaiter>(waiter), conn = conn_->shared_from_this()] {
                        asio::post(conn->strand(), [weak] {
                            if (auto w = weak.lock(); w && !w->finished) {
                                w->finished = true;
                                w->done(std::unexpected(IoError{IoErrorCode::Closed, "cancelled"}));
                            }
                        });
                    });
                }
                open_waiters_.push_back(std::move(waiter));
                conn_->request_flush(); // lets ngtcp2 send STREAMS_BLOCKED
            }

            void do_accept(std::function<void(Result<std::shared_ptr<QuicStreamState>>)> done, const CancellationToken &token) {
                if (!accept_queue_.empty()) {
                    auto st = std::move(accept_queue_.front());
                    accept_queue_.pop_front();
                    return done(std::move(st));
                }
                if (closed_) return done(std::unexpected(IoError{IoErrorCode::Closed, "connection is closed"}));
                auto waiter = std::make_shared<QuicAcceptWaiter>();
                waiter->done = std::move(done);
                if (token.can_be_cancelled()) {
                    waiter->registration = token.on_cancel([weak = std::weak_ptr<QuicAcceptWaiter>(waiter), conn = conn_->shared_from_this()] {
                        asio::post(conn->strand(), [weak] {
                            if (auto w = weak.lock(); w && !w->finished) {
                                w->finished = true;
                                w->done(std::unexpected(IoError{IoErrorCode::Closed, "cancelled"}));
                            }
                        });
                    });
                }
                accept_waiters_.push_back(std::move(waiter));
            }

            void do_receive_datagram(Promise<Result<Bytes>> promise, const CancellationToken &token) {
                if (!datagrams_in_.empty()) {
                    Bytes data = std::move(datagrams_in_.front());
                    datagrams_in_.pop_front();
                    (void)promise.set_value(Result<Bytes>(std::move(data)));
                    return;
                }
                if (closed_) {
                    (void)promise.set_value(Result<Bytes>(std::unexpected(IoError{IoErrorCode::Closed, "connection is closed"})));
                    return;
                }
                auto waiter = std::make_shared<DatagramWaiter>();
                waiter->promise = promise;
                if (token.can_be_cancelled()) {
                    waiter->registration = token.on_cancel([weak = std::weak_ptr<DatagramWaiter>(waiter), conn = conn_->shared_from_this()] {
                        asio::post(conn->strand(), [weak] {
                            if (auto w = weak.lock(); w && !w->done) {
                                w->done = true;
                                (void)w->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                            }
                        });
                    });
                }
                datagram_waiters_.push_back(std::move(waiter));
            }

            [[nodiscard]] std::shared_ptr<QuicStreamState> stream_for(std::int64_t id, std::optional<bool> bidirectional = std::nullopt) {
                if (auto it = streams_.find(id); it != streams_.end()) return it->second;
                if (closed_) return nullptr;
                auto st = std::make_shared<QuicStreamState>();
                st->id = id;
                st->bidirectional = bidirectional ? *bidirectional : ngtcp2_is_bidi_stream(id) != 0;
                streams_[id] = st;
                return st;
            }

            [[nodiscard]] bool handshake_done() const noexcept { return handshake_done_; }

          private:
            void fire_handshake(Result<void> result) {
                if (!on_handshake) return;
                auto callback = std::move(on_handshake);
                on_handshake = nullptr;
                callback(std::move(result));
            }

            void activate(QuicStreamState &st) {
                if (st.in_active || st.closed) return;
                // Find the owning shared_ptr (streams are few; the map is the source of truth).
                if (auto it = streams_.find(st.id); it != streams_.end()) {
                    st.in_active = true;
                    active_.push_back(it->second);
                }
                conn_->request_flush();
            }

            void drain_pending(QuicStreamState &st) {
                const std::size_t limit = conn_->limits().stream_send_buffer;
                while (!st.pending.empty()) {
                    auto pending = st.pending.front();
                    if (pending->done) {
                        st.pending.pop_front();
                        continue;
                    }
                    const std::uint64_t unacked = st.queued - st.acked;
                    if (unacked != 0 && unacked + pending->data.size() > limit) break; // wait for the peer to catch up
                    auto buffer = std::make_shared<Bytes>(std::move(pending->data));
                    st.out.push_back({buffer, st.queued});
                    st.queued += buffer->size();
                    pending->done = true;
                    pending->registration.reset();
                    (void)pending->promise.set_value(Result<void>());
                    st.pending.pop_front();
                    activate(st);
                }
            }

            void fail_pending(QuicStreamState &st, const IoError &error) {
                for (auto &pending : st.pending) {
                    if (pending->done) continue;
                    pending->done = true;
                    pending->registration.reset();
                    (void)pending->promise.set_value(Result<void>(std::unexpected(error)));
                }
                st.pending.clear();
            }

            /// Hands buffered bytes (or the end / reset) to waiting readers and returns flow-control credit for what was consumed.
            void serve_readers(QuicStreamState &st) {
                std::size_t consumed_total = 0;
                while (!st.readers.empty()) {
                    auto waiter = st.readers.front();
                    if (waiter->done) {
                        st.readers.pop_front();
                        continue;
                    }
                    if (st.reset_code && st.chunks.empty()) {
                        waiter->done = true;
                        waiter->registration.reset();
                        st.readers.pop_front();
                        (void)waiter->promise.set_value(Result<Bytes>(std::unexpected(
                            IoError{IoErrorCode::ConnectionReset, "stream reset by the peer (code " + std::to_string(*st.reset_code) + ")"})));
                        continue;
                    }
                    if (st.read_stopped && st.chunks.empty()) {
                        waiter->done = true;
                        waiter->registration.reset();
                        st.readers.pop_front();
                        (void)waiter->promise.set_value(Result<Bytes>(std::unexpected(IoError{IoErrorCode::Closed, "reading was stopped"})));
                        continue;
                    }
                    if (!st.chunks.empty()) {
                        Bytes out;
                        while (!st.chunks.empty() && out.size() < waiter->max) {
                            Bytes &front = st.chunks.front();
                            const std::size_t take = (std::min)(front.size(), waiter->max - out.size());
                            out.insert(out.end(), front.begin(), front.begin() + static_cast<std::ptrdiff_t>(take));
                            if (take == front.size()) st.chunks.pop_front();
                            else front.erase(front.begin(), front.begin() + static_cast<std::ptrdiff_t>(take));
                        }
                        st.buffered -= out.size();
                        consumed_total += out.size();
                        if (!st.closed && !st.fin) (void)ngtcp2_conn_extend_max_stream_offset(conn_->raw(), st.id, out.size());
                        ngtcp2_conn_extend_max_offset(conn_->raw(), out.size());
                        waiter->done = true;
                        waiter->registration.reset();
                        st.readers.pop_front();
                        (void)waiter->promise.set_value(Result<Bytes>(std::move(out)));
                        continue;
                    }
                    if (st.fin) {
                        waiter->done = true;
                        waiter->registration.reset();
                        st.readers.pop_front();
                        (void)waiter->promise.set_value(Result<Bytes>(Bytes{}));
                        continue;
                    }
                    if (st.closed) {
                        waiter->done = true;
                        waiter->registration.reset();
                        st.readers.pop_front();
                        (void)waiter->promise.set_value(Result<Bytes>(std::unexpected(IoError{IoErrorCode::Closed, "stream closed"})));
                        continue;
                    }
                    break;
                }
                if (consumed_total > 0 && !closed_) conn_->request_flush(); // carry the MAX_DATA / MAX_STREAM_DATA updates out
            }

            void deliver_accepted(const std::shared_ptr<QuicStreamState> &st) {
                while (!accept_waiters_.empty()) {
                    auto waiter = accept_waiters_.front();
                    accept_waiters_.pop_front();
                    if (waiter->finished) continue;
                    waiter->finished = true;
                    waiter->registration.reset();
                    waiter->done(st);
                    return;
                }
                accept_queue_.push_back(st);
            }

            QuicConn *conn_;
            bool handshake_done_ = false;
            bool closed_ = false;
            std::unordered_map<std::int64_t, std::shared_ptr<QuicStreamState>> streams_;
            std::deque<std::shared_ptr<QuicStreamState>> active_;
            std::deque<std::shared_ptr<QuicStreamState>> accept_queue_;
            std::deque<std::shared_ptr<QuicAcceptWaiter>> accept_waiters_;
            std::deque<std::shared_ptr<StreamOpenWaiter>> open_waiters_;
            std::deque<Bytes> datagrams_in_;
            std::deque<std::shared_ptr<DatagramWaiter>> datagram_waiters_;
        };

        /// What the public handles share. The last handle to go closes the connection.
        struct QuicHandle {
            std::shared_ptr<QuicConn> conn;
            RawApp *app = nullptr;
            SocketAddress peer;
            SocketAddress local;
            ~QuicHandle() {
                if (conn) conn->request_close(0, "connection handle released");
            }
            static QuicStream make_stream(const std::shared_ptr<QuicHandle> &handle, std::shared_ptr<QuicStreamState> state) {
                QuicStream stream;
                stream.connection_ = handle;
                stream.state_ = std::move(state);
                return stream;
            }
        };

        SocketAddress to_socket_address(const udp::endpoint &endpoint) {
            auto address = endpoint.address();
            if (address.is_v6() && address.to_v6().is_v4_mapped()) address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
            return SocketAddress{address.to_string(), endpoint.port()};
        }

        std::shared_ptr<QuicHandle> make_handle(const std::shared_ptr<QuicConn> &conn, const udp::endpoint &local) {
            auto handle = std::make_shared<QuicHandle>();
            handle->conn = conn;
            handle->app = static_cast<RawApp *>(conn->app());
            handle->peer = to_socket_address(conn->remote());
            handle->local = to_socket_address(local);
            return handle;
        }

        template <typename T>
        TaskHandle<Result<T>> immediate_error(IoError error) {
            Promise<Result<T>> promise;
            (void)promise.set_value(Result<T>(std::unexpected(std::move(error))));
            return promise.handle();
        }

        /// TLS 1.3 only, no tickets: QUIC needs 1.3, and resumption/0-RTT would add replay surface we do not want.
        void harden_quic_context(SSL_CTX *context, bool server) {
            SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION);
            SSL_CTX_set_max_proto_version(context, TLS1_3_VERSION);
            SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
            SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
            if (server) SSL_CTX_set_num_tickets(context, 0);
        }

        Result<void> check_limits(const QuicLimits &limits) {
            if (limits.idle_timeout.count() <= 0 || limits.handshake_timeout.count() <= 0 || limits.stream_receive_window < 1024 ||
                limits.connection_receive_window < limits.stream_receive_window || limits.stream_send_buffer == 0) {
                return std::unexpected(IoError{IoErrorCode::InvalidArgument, "invalid QUIC limits"});
            }
            if (limits.keep_alive.count() > 0 && limits.keep_alive >= limits.idle_timeout) {
                return std::unexpected(IoError{IoErrorCode::InvalidArgument, "keep_alive must be shorter than idle_timeout"});
            }
            return {};
        }

        struct QuicListenerState {
            std::shared_ptr<QuicEndpoint> endpoint;
            // strand-confined
            std::deque<QuicConnection> ready;
            struct Waiter {
                Promise<Result<QuicConnection>> promise;
                CancellationRegistration registration;
                bool done = false;
            };
            std::deque<std::shared_ptr<Waiter>> waiters;
            bool stopped = false;
        };

    } // namespace Detail

    using namespace Detail;

    // =================================================================================================
    // QuicStream
    // =================================================================================================
    std::int64_t QuicStream::id() const noexcept { return state_ ? state_->id : -1; }
    bool QuicStream::is_bidirectional() const noexcept { return state_ && state_->bidirectional; }

    TaskHandle<std::expected<void, IoError>> QuicStream::send(std::vector<std::byte> data, CancellationToken cancellation) {
        if (!state_) return immediate_error<void>(IoError{IoErrorCode::Closed, "invalid stream"});
        Promise<Result<void>> promise;
        auto handle = promise.handle();
        asio::post(connection_->conn->strand(), [connection = connection_, state = state_, data = std::move(data), promise, cancellation]() mutable {
            connection->app->do_send(state, std::move(data), promise, cancellation);
        });
        return handle;
    }

    TaskHandle<std::expected<void, IoError>> QuicStream::finish() {
        if (!state_) return immediate_error<void>(IoError{IoErrorCode::Closed, "invalid stream"});
        Promise<Result<void>> promise;
        auto handle = promise.handle();
        asio::post(connection_->conn->strand(), [connection = connection_, state = state_, promise]() mutable { connection->app->do_finish(state, promise); });
        return handle;
    }

    TaskHandle<std::expected<std::vector<std::byte>, IoError>> QuicStream::receive(std::size_t max_bytes, CancellationToken cancellation) {
        if (!state_) return immediate_error<Bytes>(IoError{IoErrorCode::Closed, "invalid stream"});
        Promise<Result<Bytes>> promise;
        auto handle = promise.handle();
        asio::post(connection_->conn->strand(), [connection = connection_, state = state_, max_bytes, promise, cancellation]() mutable {
            connection->app->do_receive(state, max_bytes, promise, cancellation);
        });
        return handle;
    }

    void QuicStream::reset(std::uint64_t error_code) {
        if (!state_) return;
        asio::post(connection_->conn->strand(), [connection = connection_, state = state_, error_code] { connection->app->do_reset(state, error_code); });
    }

    void QuicStream::stop_sending(std::uint64_t error_code) {
        if (!state_) return;
        asio::post(connection_->conn->strand(), [connection = connection_, state = state_, error_code] { connection->app->do_stop_sending(state, error_code); });
    }

    // =================================================================================================
    // QuicConnection
    // =================================================================================================
    TaskHandle<std::expected<QuicConnection, IoError>> QuicConnection::connect(std::string host, std::uint16_t port, QuicClientOptions options,
                                                                              CancellationToken cancellation) {
        using Out = Result<QuicConnection>;
        auto fail = [](IoError error) { return immediate_error<QuicConnection>(std::move(error)); };
        if (options.tls.alpn.empty()) return fail(IoError{IoErrorCode::InvalidArgument, "QUIC needs at least one ALPN protocol in tls.alpn"});
        if (auto limits = check_limits(options); !limits) return fail(limits.error());
        if (cancellation.is_cancelled()) {
            Promise<Out> promise;
            (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            return promise.handle();
        }
        Promise<Out> promise;
        auto handle = promise.handle();

        auto resolving = resolve_host(host, port);
        resolving.on_done([resolving, promise, host, port, options = std::move(options), cancellation]() mutable {
            try {
                auto addresses = resolving.wait();
                if (!addresses || addresses->empty()) {
                    (void)promise.set_value(Out(std::unexpected(addresses ? IoError{IoErrorCode::NotFound, "no address for " + host} : addresses.error())));
                    return;
                }
                ErrorCode ec;
                const udp::endpoint remote(asio::ip::make_address((*addresses)[0].host, ec), port);
                if (ec) {
                    (void)promise.set_value(Out(std::unexpected(IoError{IoErrorCode::InvalidArgument, "bad address " + (*addresses)[0].host})));
                    return;
                }
                auto context = make_tls_client_context(options.tls);
                if (!context) {
                    (void)promise.set_value(Out(std::unexpected(context.error())));
                    return;
                }
                harden_quic_context((*context)->native_handle(), false);

                QuicEndpointConfig config;
                config.server = false;
                config.limits = options;
                config.tls_context = *context;
                config.client_tls = options.tls;
                config.make_app = [](QuicConn &conn) -> std::shared_ptr<QuicApp> { return std::make_shared<RawApp>(conn); };
                auto endpoint = QuicEndpoint::create(std::move(config), udp::endpoint(remote.protocol(), 0));
                if (!endpoint) {
                    (void)promise.set_value(Out(std::unexpected(endpoint.error())));
                    return;
                }
                auto conn = (*endpoint)->connect(remote, host);
                if (!conn) {
                    (void)promise.set_value(Out(std::unexpected(conn.error())));
                    return;
                }
                auto shared = make_handle(*conn, (*endpoint)->local());
                auto cancel_registration = std::make_shared<CancellationRegistration>();
                shared->app->wait_for_confirmation = !options.tls.client_certificate_chain_pem.empty();
                shared->app->on_handshake = [promise, shared, cancel_registration](Result<void> result) mutable {
                    cancel_registration->reset();
                    if (result) (void)promise.set_value(Out(QuicConnection(shared)));
                    else (void)promise.set_value(Out(std::unexpected(result.error())));
                };
                (*endpoint)->start();
                (*conn)->request_flush(); // begins the handshake, now that the callbacks are in place
                if (cancellation.can_be_cancelled()) {
                    *cancel_registration = cancellation.on_cancel([weak = std::weak_ptr<QuicConn>(*conn), promise]() mutable {
                        if (auto c = weak.lock()) {
                            asio::post(c->strand(), [c, promise]() mutable {
                                if (!c->handshake_complete()) {
                                    (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                                    c->close_now(0, "cancelled");
                                }
                            });
                        }
                    });
                }
            } catch (...) {
                (void)promise.set_value(Out(std::unexpected(IoError{IoErrorCode::Unknown, "QUIC connect failed"})));
            }
        });
        return handle;
    }

    TaskHandle<std::expected<QuicStream, IoError>> QuicConnection::open_stream(bool bidirectional, CancellationToken cancellation) {
        if (!handle_) return immediate_error<QuicStream>(IoError{IoErrorCode::Closed, "invalid connection"});
        Promise<Result<QuicStream>> promise;
        auto result = promise.handle();
        asio::post(handle_->conn->strand(), [handle = handle_, bidirectional, promise, cancellation]() mutable {
            handle->app->do_open(
                bidirectional,
                [handle, promise](Result<std::shared_ptr<QuicStreamState>> opened) mutable {
                    if (!opened) (void)promise.set_value(Result<QuicStream>(std::unexpected(opened.error())));
                    else (void)promise.set_value(Result<QuicStream>(QuicHandle::make_stream(handle, std::move(*opened))));
                },
                cancellation);
        });
        return result;
    }

    TaskHandle<std::expected<QuicStream, IoError>> QuicConnection::accept_stream(CancellationToken cancellation) {
        if (!handle_) return immediate_error<QuicStream>(IoError{IoErrorCode::Closed, "invalid connection"});
        Promise<Result<QuicStream>> promise;
        auto result = promise.handle();
        asio::post(handle_->conn->strand(), [handle = handle_, promise, cancellation]() mutable {
            handle->app->do_accept(
                [handle, promise](Result<std::shared_ptr<QuicStreamState>> accepted) mutable {
                    if (!accepted) (void)promise.set_value(Result<QuicStream>(std::unexpected(accepted.error())));
                    else (void)promise.set_value(Result<QuicStream>(QuicHandle::make_stream(handle, std::move(*accepted))));
                },
                cancellation);
        });
        return result;
    }

    std::expected<void, IoError> QuicConnection::send_datagram(std::vector<std::byte> data) {
        if (!handle_) return std::unexpected(IoError{IoErrorCode::Closed, "invalid connection"});
        if (!handle_->conn->limits().datagrams) return std::unexpected(IoError{IoErrorCode::Unsupported, "datagrams are disabled in the options"});
        const std::size_t limit = handle_->conn->max_datagram_size();
        if (limit == 0) return std::unexpected(IoError{IoErrorCode::Unsupported, "the peer does not accept datagrams (or the handshake is not done)"});
        if (data.size() > limit) return std::unexpected(IoError{IoErrorCode::LimitExceeded, "datagram larger than max_datagram_size() (" + std::to_string(limit) + ")"});
        {
            auto guard = handle_->app->datagrams_out.lock();
            if (guard->queue.size() >= handle_->conn->limits().datagram_send_queue) return std::unexpected(IoError{IoErrorCode::LimitExceeded, "datagram send queue is full"});
            OutDatagram datagram;
            datagram.data.resize(data.size());
            std::memcpy(datagram.data.data(), data.data(), data.size());
            datagram.id = guard->next_id++;
            datagram.queued = std::chrono::steady_clock::now();
            guard->queue.push_back(std::move(datagram));
        }
        handle_->conn->request_flush();
        return {};
    }

    TaskHandle<std::expected<std::vector<std::byte>, IoError>> QuicConnection::receive_datagram(CancellationToken cancellation) {
        if (!handle_) return immediate_error<Bytes>(IoError{IoErrorCode::Closed, "invalid connection"});
        Promise<Result<Bytes>> promise;
        auto result = promise.handle();
        asio::post(handle_->conn->strand(), [handle = handle_, promise, cancellation]() mutable { handle->app->do_receive_datagram(promise, cancellation); });
        return result;
    }

    std::size_t QuicConnection::max_datagram_size() const { return handle_ ? handle_->conn->max_datagram_size() : 0; }

    void QuicConnection::close(std::uint64_t error_code, std::string reason) {
        if (handle_) handle_->conn->request_close(error_code, std::move(reason));
    }

    TaskHandle<QuicCloseInfo> QuicConnection::closed() {
        Promise<QuicCloseInfo> promise;
        auto result = promise.handle();
        if (!handle_) {
            QuicCloseInfo info;
            info.reason = "invalid connection";
            (void)promise.set_value(std::move(info));
            return result;
        }
        auto guard = handle_->app->close_state.lock();
        if (guard->closed) (void)promise.set_value(QuicCloseInfo(guard->info));
        else guard->waiters.push_back(promise);
        return result;
    }

    bool QuicConnection::is_open() const {
        if (!handle_) return false;
        auto guard = handle_->app->close_state.lock();
        return !guard->closed;
    }

    std::expected<SocketAddress, IoError> QuicConnection::peer_address() const {
        if (!handle_) return std::unexpected(IoError{IoErrorCode::Closed, "invalid connection"});
        return handle_->peer;
    }
    std::expected<SocketAddress, IoError> QuicConnection::local_address() const {
        if (!handle_) return std::unexpected(IoError{IoErrorCode::Closed, "invalid connection"});
        return handle_->local;
    }
    std::expected<TlsInfo, IoError> QuicConnection::tls_info() const {
        if (!handle_) return std::unexpected(IoError{IoErrorCode::Closed, "invalid connection"});
        TlsInfo info = handle_->conn->tls_info();
        if (info.protocol.empty()) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "TLS details are not available yet"});
        return info;
    }
    QuicStats QuicConnection::stats() const { return handle_ ? handle_->conn->stats() : QuicStats{}; }

    // =================================================================================================
    // QuicListener
    // =================================================================================================
    QuicListener::~QuicListener() noexcept { stop(); }
    QuicListener &QuicListener::operator=(QuicListener &&other) noexcept {
        if (this != &other) {
            stop();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    TaskHandle<std::expected<QuicListener, IoError>> QuicListener::listen(QuicServerOptions options) {
        auto fail = [](IoError error) { return immediate_error<QuicListener>(std::move(error)); };
        if (options.tls.alpn.empty()) return fail(IoError{IoErrorCode::InvalidArgument, "QUIC needs at least one ALPN protocol in tls.alpn"});
        if (auto limits = check_limits(options); !limits) return fail(limits.error());
        if (options.max_connections == 0 || options.max_connections_per_address == 0) return fail(IoError{IoErrorCode::InvalidArgument, "invalid connection limits"});
        auto context = make_tls_server_context(options.tls);
        if (!context) return fail(context.error());
        harden_quic_context((*context)->native_handle(), true);

        auto state = std::make_shared<QuicListenerState>();
        QuicEndpointConfig config;
        config.server = true;
        config.limits = options;
        config.tls_context = *context;
        config.pinned_client_sha256 = options.tls.pinned_client_sha256;
        config.max_connections = options.max_connections;
        config.max_connections_per_address = options.max_connections_per_address;
        config.require_address_validation = options.require_address_validation;
        config.make_app = [](QuicConn &conn) -> std::shared_ptr<QuicApp> { return std::make_shared<RawApp>(conn); };
        config.on_ready = [weak = std::weak_ptr<QuicListenerState>(state)](const std::shared_ptr<QuicConn> &conn) {
            auto listener = weak.lock();
            if (!listener || listener->stopped) {
                conn->close_now(0, "server is shutting down");
                return;
            }
            QuicConnection accepted(make_handle(conn, listener->endpoint->local()));
            while (!listener->waiters.empty()) {
                auto waiter = listener->waiters.front();
                listener->waiters.pop_front();
                if (waiter->done) continue;
                waiter->done = true;
                waiter->registration.reset();
                (void)waiter->promise.set_value(Result<QuicConnection>(std::move(accepted)));
                return;
            }
            listener->ready.push_back(std::move(accepted));
        };

        udp::endpoint bind_to;
        ErrorCode ec;
        if (options.bind_address.empty()) {
            bind_to = udp::endpoint(udp::v6(), options.port);
        } else {
            bind_to = udp::endpoint(asio::ip::make_address(options.bind_address, ec), options.port);
            if (ec) return fail(IoError{IoErrorCode::InvalidArgument, "bad bind address: " + options.bind_address});
        }
        auto endpoint = QuicEndpoint::create(config, bind_to);
        if (!endpoint && options.bind_address.empty()) endpoint = QuicEndpoint::create(config, udp::endpoint(udp::v4(), options.port)); // no IPv6 here
        if (!endpoint) return fail(endpoint.error());
        state->endpoint = *endpoint;
        state->endpoint->start();
        QuicListener listener;
        listener.state_ = state;
        Promise<Result<QuicListener>> promise;
        (void)promise.set_value(Result<QuicListener>(std::move(listener)));
        return promise.handle();
    }

    TaskHandle<std::expected<QuicConnection, IoError>> QuicListener::accept(CancellationToken cancellation) {
        if (!state_) return immediate_error<QuicConnection>(IoError{IoErrorCode::Closed, "listener not started"});
        Promise<Result<QuicConnection>> promise;
        auto result = promise.handle();
        asio::post(state_->endpoint->strand(), [state = state_, promise, cancellation]() mutable {
            if (!state->ready.empty()) {
                QuicConnection connection = std::move(state->ready.front());
                state->ready.pop_front();
                (void)promise.set_value(Result<QuicConnection>(std::move(connection)));
                return;
            }
            if (state->stopped) {
                (void)promise.set_value(Result<QuicConnection>(std::unexpected(IoError{IoErrorCode::Closed, "listener stopped"})));
                return;
            }
            auto waiter = std::make_shared<QuicListenerState::Waiter>();
            waiter->promise = promise;
            if (cancellation.can_be_cancelled()) {
                waiter->registration = cancellation.on_cancel([weak = std::weak_ptr<QuicListenerState::Waiter>(waiter), state] {
                    asio::post(state->endpoint->strand(), [weak] {
                        if (auto w = weak.lock(); w && !w->done) {
                            w->done = true;
                            (void)w->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                        }
                    });
                });
            }
            state->waiters.push_back(std::move(waiter));
        });
        return result;
    }

    std::expected<std::uint16_t, IoError> QuicListener::local_port() const {
        if (!state_) return std::unexpected(IoError{IoErrorCode::Closed, "listener not started"});
        return state_->endpoint->local_port();
    }

    QuicListenerMetrics QuicListener::metrics() const noexcept {
        QuicListenerMetrics m;
        if (!state_) return m;
        auto &e = state_->endpoint->metrics();
        m.connections_accepted = e.accepted.load();
        m.connections_active = e.active.load();
        m.retries_sent = e.retries.load();
        m.rejected_limit = e.rejected_limit.load();
        m.invalid_tokens = e.invalid_tokens.load();
        m.packets_dropped = e.dropped.load();
        m.handshakes_failed = e.handshakes_failed.load();
        return m;
    }

    void QuicListener::stop() noexcept {
        if (!state_) return;
        auto state = state_;
        asio::post(state->endpoint->strand(), [state] {
            if (state->stopped) return;
            state->stopped = true;
            for (auto &waiter : state->waiters) {
                if (waiter->done) continue;
                waiter->done = true;
                (void)waiter->promise.set_value(Result<QuicConnection>(std::unexpected(IoError{IoErrorCode::Closed, "listener stopped"})));
            }
            state->waiters.clear();
            state->ready.clear(); // dropping the handles closes the not-yet-accepted connections
        });
        state->endpoint->stop();
    }

} // namespace SFT::Async

#else

namespace SFT::Async {
    bool quic_available() noexcept { return false; }
    namespace {
        template <typename T>
        TaskHandle<std::expected<T, IoError>> unsupported() {
            Promise<std::expected<T, IoError>> promise;
            (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "QUIC is not available in this build (needs ngtcp2 and OpenSSL 3.5+)"}));
            return promise.handle();
        }
    } // namespace
    std::int64_t QuicStream::id() const noexcept { return -1; }
    bool QuicStream::is_bidirectional() const noexcept { return false; }
    TaskHandle<std::expected<void, IoError>> QuicStream::send(std::vector<std::byte>, CancellationToken) { return unsupported<void>(); }
    TaskHandle<std::expected<void, IoError>> QuicStream::finish() { return unsupported<void>(); }
    TaskHandle<std::expected<std::vector<std::byte>, IoError>> QuicStream::receive(std::size_t, CancellationToken) { return unsupported<std::vector<std::byte>>(); }
    void QuicStream::reset(std::uint64_t) {}
    void QuicStream::stop_sending(std::uint64_t) {}
    TaskHandle<std::expected<QuicConnection, IoError>> QuicConnection::connect(std::string, std::uint16_t, QuicClientOptions, CancellationToken) { return unsupported<QuicConnection>(); }
    TaskHandle<std::expected<QuicStream, IoError>> QuicConnection::open_stream(bool, CancellationToken) { return unsupported<QuicStream>(); }
    TaskHandle<std::expected<QuicStream, IoError>> QuicConnection::accept_stream(CancellationToken) { return unsupported<QuicStream>(); }
    std::expected<void, IoError> QuicConnection::send_datagram(std::vector<std::byte>) { return std::unexpected(IoError{IoErrorCode::Unsupported, "QUIC is not available"}); }
    TaskHandle<std::expected<std::vector<std::byte>, IoError>> QuicConnection::receive_datagram(CancellationToken) { return unsupported<std::vector<std::byte>>(); }
    std::size_t QuicConnection::max_datagram_size() const { return 0; }
    void QuicConnection::close(std::uint64_t, std::string) {}
    TaskHandle<QuicCloseInfo> QuicConnection::closed() {
        Promise<QuicCloseInfo> promise;
        (void)promise.set_value(QuicCloseInfo{});
        return promise.handle();
    }
    bool QuicConnection::is_open() const { return false; }
    std::expected<SocketAddress, IoError> QuicConnection::peer_address() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "QUIC is not available"}); }
    std::expected<SocketAddress, IoError> QuicConnection::local_address() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "QUIC is not available"}); }
    std::expected<TlsInfo, IoError> QuicConnection::tls_info() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "QUIC is not available"}); }
    QuicStats QuicConnection::stats() const { return {}; }
    QuicListener::~QuicListener() noexcept {}
    QuicListener &QuicListener::operator=(QuicListener &&) noexcept = default;
    TaskHandle<std::expected<QuicListener, IoError>> QuicListener::listen(QuicServerOptions) { return unsupported<QuicListener>(); }
    TaskHandle<std::expected<QuicConnection, IoError>> QuicListener::accept(CancellationToken) { return unsupported<QuicConnection>(); }
    std::expected<std::uint16_t, IoError> QuicListener::local_port() const { return std::unexpected(IoError{IoErrorCode::Unsupported, "QUIC is not available"}); }
    QuicListenerMetrics QuicListener::metrics() const noexcept { return {}; }
    void QuicListener::stop() noexcept {}
} // namespace SFT::Async

#endif
