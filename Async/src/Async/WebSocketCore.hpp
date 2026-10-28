#pragma once

// Private: the WebSocket state machine shared by the client, the standalone WebSocketServer and the HTTP
// server's upgrade path.

#include <Async/BeastStream.hpp>
#include <Async/WebSocket.hpp>

#include <boost/beast/websocket.hpp>

#include <deque>
#include <functional>
#include <optional>

namespace SFT::Async::Detail {

    namespace beast = boost::beast;
    namespace websocket = beast::websocket;

    // Beast customization points: how WebSocket tears down a CoreStream transport. The reactor connection has no
    // half-close handshake of its own, so teardown simply closes it.
    inline void teardown(beast::role_type, CoreStream &stream, ErrorCode &ec) {
        stream.core()->request_close();
        ec = {};
    }
    template <typename Handler>
    void async_teardown(beast::role_type, CoreStream &stream, Handler &&handler) {
        stream.core()->request_close();
        asio::post(stream.get_executor(), [h = std::forward<Handler>(handler)]() mutable { h(ErrorCode{}); });
    }

    class WsOps {
      public:
        virtual ~WsOps() = default;
        virtual TaskHandle<Result<WebSocketMessage>> receive(CancellationToken cancellation) = 0;
        virtual TaskHandle<Result<void>> send(bool text, std::vector<std::byte> data) = 0;
        virtual TaskHandle<Result<void>> ping(std::vector<std::byte> payload) = 0;
        virtual TaskHandle<Result<void>> close(std::uint16_t code, std::string reason) = 0;
        virtual void abort() noexcept = 0;
        [[nodiscard]] virtual bool is_open() const noexcept = 0;
        [[nodiscard]] virtual std::optional<WebSocketCloseInfo> close_info() const = 0;
        [[nodiscard]] virtual Result<SocketAddress> peer_address() const = 0;
        [[nodiscard]] virtual Result<TlsInfo> tls_info() const = 0;

        WebSocketRequest request;
    };

    /// The only door through which internal code creates a public `WebSocket` from a session.
    struct WsFactory {
        static WebSocket make(std::shared_ptr<WsOps> ops) { return WebSocket(std::move(ops)); }
    };

    template <typename Next>
    class WsSession final : public WsOps, public std::enable_shared_from_this<WsSession<Next>> {
      public:
        using Ws = websocket::stream<Next>;

        template <typename... Args>
        explicit WsSession(WebSocketOptions socket_options, Args &&...args)
            : ws(std::forward<Args>(args)...), options(socket_options), inbox_buffer(socket_options.max_message_bytes + 4096) {
            ws.read_message_max(options.max_message_bytes);
            if (options.compression) {
                websocket::permessage_deflate deflate;
                deflate.client_enable = true;
                deflate.server_enable = true;
                deflate.client_no_context_takeover = true;
                deflate.server_no_context_takeover = true;
                ws.set_option(deflate);
            }
            const auto handshake = options.handshake_timeout;
            if (options.idle_timeout.count() > 0) {
                ws.set_option(websocket::stream_base::timeout{handshake, options.idle_timeout, true});
            } else {
                ws.set_option(websocket::stream_base::timeout{handshake, websocket::stream_base::none(), false});
            }
        }

        ~WsSession() override {
            if (live) live->fetch_sub(1, std::memory_order_acq_rel);
        }

        Ws ws;
        WebSocketOptions options;
        std::shared_ptr<std::atomic<std::size_t>> live;
        std::function<Result<SocketAddress>()> peer_provider;
        std::function<Result<TlsInfo>()> tls_provider;

        [[nodiscard]] auto executor() { return ws.get_executor(); }

        /// Called once the upgrade is complete: starts the background reader.
        void start() {
            open_.store(true, std::memory_order_release);
            asio::post(executor(), [self = this->shared_from_this()] { self->read_loop(); });
        }

        // ---- WsOps -----------------------------------------------------------------------------
        TaskHandle<Result<WebSocketMessage>> receive(CancellationToken cancellation) override {
            if (cancellation.is_cancelled()) return cancelled_handle<WebSocketMessage>();
            auto reader = std::make_shared<Reader>();
            auto handle = reader->promise.handle();
            asio::post(executor(), [self = this->shared_from_this(), reader, cancellation] {
                if (cancellation.can_be_cancelled()) {
                    reader->registration = cancellation.on_cancel([weak_self = std::weak_ptr<WsSession>(self), weak_reader = std::weak_ptr<Reader>(reader)] {
                        auto session = weak_self.lock();
                        if (!session) return;
                        asio::post(session->executor(), [session, weak_reader] {
                            if (auto r = weak_reader.lock(); r && !r->done) {
                                r->done = true;
                                (void)r->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                            }
                        });
                    });
                }
                self->readers_.push_back(reader);
                self->deliver();
            });
            return handle;
        }

        TaskHandle<Result<void>> send(bool text, std::vector<std::byte> data) override {
            auto item = std::make_shared<Outgoing>();
            item->kind = text ? Outgoing::Kind::Text : Outgoing::Kind::Binary;
            item->data = std::move(data);
            return enqueue(std::move(item));
        }

        TaskHandle<Result<void>> ping(std::vector<std::byte> payload) override {
            if (payload.size() > 125) return failed<void>(IoError{IoErrorCode::InvalidArgument, "ping payload is limited to 125 bytes"});
            auto item = std::make_shared<Outgoing>();
            item->kind = Outgoing::Kind::Ping;
            item->data = std::move(payload);
            return enqueue(std::move(item));
        }

        TaskHandle<Result<void>> close(std::uint16_t code, std::string reason) override {
            if (reason.size() > 123) return failed<void>(IoError{IoErrorCode::InvalidArgument, "close reason is limited to 123 bytes"});
            auto item = std::make_shared<Outgoing>();
            item->kind = Outgoing::Kind::Close;
            item->close_code = code;
            item->reason = std::move(reason);
            return enqueue(std::move(item));
        }

        void abort() noexcept override {
            try {
                asio::post(executor(), [self = this->shared_from_this()] { self->terminate(IoError{IoErrorCode::Closed, "WebSocket aborted"}); });
            } catch (...) {
            }
        }

        [[nodiscard]] bool is_open() const noexcept override { return open_.load(std::memory_order_acquire); }

        [[nodiscard]] std::optional<WebSocketCloseInfo> close_info() const override {
            auto guard = close_info_.lock();
            return *guard;
        }

        [[nodiscard]] Result<SocketAddress> peer_address() const override {
            return peer_provider ? peer_provider() : Result<SocketAddress>(std::unexpected(IoError{IoErrorCode::Unsupported, "no peer address"}));
        }

        [[nodiscard]] Result<TlsInfo> tls_info() const override {
            return tls_provider ? tls_provider() : Result<TlsInfo>(std::unexpected(IoError{IoErrorCode::InvalidArgument, "connection is not using TLS"}));
        }

      private:
        struct Reader {
            Promise<Result<WebSocketMessage>> promise;
            bool done = false;
            CancellationRegistration registration;
        };
        struct Outgoing {
            enum class Kind { Text, Binary, Ping, Close } kind = Kind::Binary;
            std::vector<std::byte> data;
            std::uint16_t close_code = 1000;
            std::string reason;
            Promise<Result<void>> promise;
            bool done = false;
        };

        static IoError map_error(const ErrorCode &ec) {
            if (ec == websocket::error::closed) return IoError{IoErrorCode::Closed, "WebSocket closed by peer"};
            if (ec == websocket::error::message_too_big || ec == beast::error::timeout) {
                return IoError{ec == beast::error::timeout ? IoErrorCode::TimedOut : IoErrorCode::LimitExceeded,
                               ec == beast::error::timeout ? "WebSocket idle timeout" : "incoming WebSocket message exceeds the configured limit"};
            }
            if (ec.category() == websocket::make_error_code(websocket::error::bad_opcode).category()) {
                return IoError{IoErrorCode::ProtocolError, "WebSocket protocol error: " + ec.message()};
            }
            return to_io_error("websocket", ec);
        }

        TaskHandle<Result<void>> enqueue(std::shared_ptr<Outgoing> item) {
            auto handle = item->promise.handle();
            asio::post(executor(), [self = this->shared_from_this(), item] {
                if (self->terminal_) {
                    (void)item->promise.set_value(Result<void>(std::unexpected(*self->terminal_)));
                    return;
                }
                if (self->queued_bytes_ + item->data.size() > self->options.max_queued_send_bytes) {
                    (void)item->promise.set_value(Result<void>(std::unexpected(
                        IoError{IoErrorCode::LimitExceeded, "WebSocket send queue is full; await sends to pace the peer"})));
                    return;
                }
                self->queued_bytes_ += item->data.size();
                self->writes_.push_back(item);
                self->pump_writes();
            });
            return handle;
        }

        void pump_writes() {
            if (writing_ || writes_.empty() || terminal_) return;
            auto item = writes_.front();
            writing_ = true;
            auto done = [self = this->shared_from_this(), item](const ErrorCode &ec, std::size_t) { self->on_written(item, ec); };
            switch (item->kind) {
                case Outgoing::Kind::Text:
                case Outgoing::Kind::Binary:
                    ws.text(item->kind == Outgoing::Kind::Text);
                    ws.async_write(asio::buffer(item->data), std::move(done));
                    break;
                case Outgoing::Kind::Ping: {
                    websocket::ping_data payload;
                    payload.assign(reinterpret_cast<const char *>(item->data.data()), reinterpret_cast<const char *>(item->data.data()) + item->data.size());
                    ws.async_ping(payload, [done = std::move(done)](const ErrorCode &ec) { done(ec, 0); });
                    break;
                }
                case Outgoing::Kind::Close: {
                    websocket::close_reason reason(static_cast<websocket::close_code>(item->close_code), item->reason);
                    ws.async_close(reason, [done = std::move(done)](const ErrorCode &ec) { done(ec, 0); });
                    break;
                }
            }
        }

        void on_written(const std::shared_ptr<Outgoing> &item, const ErrorCode &ec) {
            writing_ = false;
            if (!writes_.empty() && writes_.front() == item) {
                writes_.pop_front();
                queued_bytes_ -= std::min(queued_bytes_, item->data.size());
            }
            if (ec && ec != websocket::error::closed) {
                if (!item->done) {
                    item->done = true;
                    (void)item->promise.set_value(Result<void>(std::unexpected(map_error(ec))));
                }
                terminate(map_error(ec));
                return;
            }
            if (item->kind == Outgoing::Kind::Close) {
                {
                    auto guard = close_info_.lock();
                    if (!guard->has_value()) *guard = WebSocketCloseInfo{item->close_code, item->reason};
                }
                // Mark the socket closed *before* telling the caller the close completed: a caller that checks `is_open()`
                // right after awaiting `close()` must see false.
                terminate(IoError{IoErrorCode::Closed, "WebSocket closed"});
                if (!item->done) {
                    item->done = true;
                    (void)item->promise.set_value(Result<void>());
                }
                return;
            }
            if (!item->done) {
                item->done = true;
                (void)item->promise.set_value(Result<void>());
            }
            pump_writes();
        }

        void read_loop() {
            if (reading_ || terminal_ || inbox_.size() >= options.inbox_messages) return;
            reading_ = true;
            ws.async_read(inbox_buffer, [self = this->shared_from_this()](const ErrorCode &ec, std::size_t) { self->on_read(ec); });
        }

        void on_read(const ErrorCode &ec) {
            reading_ = false;
            if (ec) {
                if (ec == websocket::error::closed) {
                    const auto reason = ws.reason();
                    auto guard = close_info_.lock();
                    if (!guard->has_value()) *guard = WebSocketCloseInfo{static_cast<std::uint16_t>(reason.code), std::string(reason.reason.c_str())};
                }
                terminate(map_error(ec));
                return;
            }
            WebSocketMessage message;
            message.is_text = ws.got_text();
            message.data.resize(inbox_buffer.size());
            asio::buffer_copy(asio::buffer(message.data), inbox_buffer.data());
            inbox_buffer.consume(inbox_buffer.size());
            inbox_.push_back(std::move(message));
            deliver();
        }

        void deliver() {
            while (!readers_.empty()) {
                auto reader = readers_.front();
                if (reader->done) {
                    readers_.pop_front();
                    continue;
                }
                if (!inbox_.empty()) {
                    reader->done = true;
                    reader->registration.reset();
                    readers_.pop_front();
                    auto message = std::move(inbox_.front());
                    inbox_.pop_front();
                    (void)reader->promise.set_value(Result<WebSocketMessage>(std::move(message)));
                    continue;
                }
                if (terminal_) {
                    reader->done = true;
                    reader->registration.reset();
                    readers_.pop_front();
                    (void)reader->promise.set_value(Result<WebSocketMessage>(std::unexpected(*terminal_)));
                    continue;
                }
                break;
            }
            read_loop();
        }

        /// The connection is over: remember why, fail what is queued, drop the transport. Buffered inbound
        /// messages stay readable.
        void terminate(IoError reason) {
            if (terminal_) return;
            terminal_ = std::move(reason);
            open_.store(false, std::memory_order_release);
            auto queued = std::move(writes_);
            writes_.clear();
            queued_bytes_ = 0;
            for (auto &item : queued) {
                if (!item->done) {
                    item->done = true;
                    (void)item->promise.set_value(Result<void>(std::unexpected(*terminal_)));
                }
            }
            beast::close_socket(beast::get_lowest_layer(ws));
            deliver();
        }

        std::atomic<bool> open_{false};
        std::optional<IoError> terminal_;
        bool reading_ = false;
        bool writing_ = false;
        beast::flat_buffer inbox_buffer;
        std::deque<WebSocketMessage> inbox_;
        std::deque<std::shared_ptr<Reader>> readers_;
        std::deque<std::shared_ptr<Outgoing>> writes_;
        std::size_t queued_bytes_ = 0;
        mutable Mutex<std::optional<WebSocketCloseInfo>> close_info_;
    };

} // namespace SFT::Async::Detail
