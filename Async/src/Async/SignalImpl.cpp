#include <Async/Signal.hpp>

#include <Async/Promise.hpp>

#include <csignal>
#include <vector>

#if !defined(STURDY_PLATFORM_WEB)

#include <Async/NetInternal.hpp>

namespace SFT::Async {

    namespace {
        struct SignalOp : std::enable_shared_from_this<SignalOp> {
            Detail::asio::signal_set set{Detail::reactor()};
            Promise<std::expected<int, IoError>> promise;
            CancellationRegistration registration;
        };
    } // namespace

    TaskHandle<std::expected<int, IoError>> wait_for_signal(std::initializer_list<int> signals, CancellationToken cancellation) {
        return wait_for_signal(std::span<const int>(signals.begin(), signals.size()), std::move(cancellation));
    }

    TaskHandle<std::expected<int, IoError>> wait_for_signal(std::span<const int> signals, CancellationToken cancellation) {
        auto op = std::make_shared<SignalOp>();
        auto handle = op->promise.handle();
        if (signals.size() == 0) {
            (void)op->promise.set_value(std::unexpected(IoError{IoErrorCode::InvalidArgument, "no signals given"}));
            return handle;
        }
        if (cancellation.is_cancelled()) {
            (void)op->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            return handle;
        }
        Detail::ErrorCode error;
        for (int number : signals) {
            op->set.add(number, error);
            if (error) {
                (void)op->promise.set_value(std::unexpected(Detail::to_io_error("install signal handler", error)));
                return handle;
            }
        }
        op->set.async_wait([op](const Detail::ErrorCode &ec, int number) {
            op->registration.reset();
            if (ec == Detail::asio::error::operation_aborted) {
                (void)op->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            } else if (ec) {
                (void)op->promise.set_value(std::unexpected(Detail::to_io_error("signal wait", ec)));
            } else {
                (void)op->promise.set_value(number);
            }
        });
        if (cancellation.can_be_cancelled()) {
            op->registration = cancellation.on_cancel([weak = std::weak_ptr<SignalOp>(op)] {
                if (auto self = weak.lock()) Detail::asio::post(Detail::reactor(), [self] { self->set.cancel(); });
            });
        }
        return handle;
    }

    TaskHandle<std::expected<int, IoError>> ctrl_c(CancellationToken cancellation) { return wait_for_signal({SIGINT}, std::move(cancellation)); }
    TaskHandle<std::expected<int, IoError>> shutdown_signal(CancellationToken cancellation) { return wait_for_signal({SIGINT, SIGTERM}, std::move(cancellation)); }

} // namespace SFT::Async

#else

namespace SFT::Async {
    namespace {
        TaskHandle<std::expected<int, IoError>> unsupported_signal() {
            Promise<std::expected<int, IoError>> promise;
            (void)promise.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "process signals are not available on Web"}));
            return promise.handle();
        }
    } // namespace
    TaskHandle<std::expected<int, IoError>> wait_for_signal(std::initializer_list<int>, CancellationToken) { return unsupported_signal(); }
    TaskHandle<std::expected<int, IoError>> wait_for_signal(std::span<const int>, CancellationToken) { return unsupported_signal(); }
    TaskHandle<std::expected<int, IoError>> ctrl_c(CancellationToken) { return unsupported_signal(); }
    TaskHandle<std::expected<int, IoError>> shutdown_signal(CancellationToken) { return unsupported_signal(); }
} // namespace SFT::Async

#endif
