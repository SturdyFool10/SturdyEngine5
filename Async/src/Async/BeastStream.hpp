#pragma once

// Adapts a reactor-side TcpConnection (plain or TLS) to the Boost.Beast stream concepts, so Beast's HTTP and
// WebSocket algorithms run on the same connections as everything else in Async. Private to .cpp files.

#include <Async/NetCore.hpp>

#include <boost/beast/core.hpp>

namespace SFT::Async::Detail {

    [[nodiscard]] inline ErrorCode to_error_code(const IoError &error) {
        switch (error.code) {
            case IoErrorCode::Closed: return asio::error::operation_aborted;
            case IoErrorCode::ConnectionReset: return asio::error::connection_reset;
            case IoErrorCode::TimedOut: return asio::error::timed_out;
            case IoErrorCode::TlsFailure: return asio::ssl::error::stream_truncated;
            case IoErrorCode::UnexpectedEof: return asio::error::eof;
            default: return asio::error::fault;
        }
    }

    class CoreStream {
      public:
        using executor_type = Strand;

        explicit CoreStream(std::shared_ptr<StreamCore> core) : core_(std::move(core)) {}

        [[nodiscard]] executor_type get_executor() noexcept { return core_->strand; }
        [[nodiscard]] const std::shared_ptr<StreamCore> &core() const noexcept { return core_; }

        template <typename MutableBufferSequence, typename ReadHandler>
        auto async_read_some(const MutableBufferSequence &buffers, ReadHandler &&handler) {
            return asio::async_initiate<ReadHandler, void(ErrorCode, std::size_t)>(
                [](auto completion, std::shared_ptr<StreamCore> core, MutableBufferSequence target) {
                    using Completion = std::decay_t<decltype(completion)>;
                    const std::size_t capacity = asio::buffer_size(target);
                    auto shared = std::make_shared<Completion>(std::move(completion));
                    if (capacity == 0) {
                        asio::post(core->strand, [shared] { (*shared)(ErrorCode{}, std::size_t{0}); });
                        return;
                    }
                    auto waiter = std::make_shared<ReadWaiter>();
                    waiter->kind = ReadWaiter::Kind::Some;
                    waiter->limit = capacity;
                    waiter->callback = [shared, target](Result<Bytes> result) {
                        if (!result) return (*shared)(to_error_code(result.error()), std::size_t{0});
                        if (result->empty()) return (*shared)(ErrorCode(asio::error::eof), std::size_t{0});
                        const std::size_t copied = asio::buffer_copy(target, asio::buffer(*result));
                        (*shared)(ErrorCode{}, copied);
                    };
                    asio::post(core->strand, [core, waiter] { core->add_waiter(waiter, {}); });
                },
                handler, core_, buffers);
        }

        template <typename ConstBufferSequence, typename WriteHandler>
        auto async_write_some(const ConstBufferSequence &buffers, WriteHandler &&handler) {
            return asio::async_initiate<WriteHandler, void(ErrorCode, std::size_t)>(
                [](auto completion, std::shared_ptr<StreamCore> core, ConstBufferSequence source) {
                    using Completion = std::decay_t<decltype(completion)>;
                    auto shared = std::make_shared<Completion>(std::move(completion));
                    auto op = std::make_shared<WriteOp>();
                    const std::size_t size = asio::buffer_size(source);
                    op->data.resize(size);
                    (void)asio::buffer_copy(asio::buffer(op->data), source);
                    op->finish = [shared, size](Result<std::size_t> result) {
                        if (!result) return (*shared)(to_error_code(result.error()), std::size_t{0});
                        (*shared)(ErrorCode{}, size);
                    };
                    op->on_cancelled = [] {};
                    asio::post(core->strand, [core, op] { core->add_write(op, {}); });
                },
                handler, core_, buffers);
        }

        /// Beast's customization point: how to abort the transport when a timeout or protocol error demands it.
        friend void beast_close_socket(CoreStream &stream) { stream.core_->request_close(); }

      private:
        std::shared_ptr<StreamCore> core_;
    };

} // namespace SFT::Async::Detail
