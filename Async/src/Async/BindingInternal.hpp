#pragma once

// Private helpers shared by the Binding*.cpp files: typed object holders, operation launching, and the conversions between
// the flat binding types and Async's own.

#include <Async/Binding.hpp>

#include <Async/Cancellation.hpp>
#include <Async/Channel.hpp>
#include <Async/Http.hpp>
#include <Async/Mutex.hpp>
#include <Async/Promise.hpp>
#include <Async/Sync.hpp>
#include <Async/Task.hpp>
#include <Async/Timer.hpp>
#include <Async/Tls.hpp>

#include <cstring>
#include <expected>
#include <type_traits>

namespace SFT::Async::Binding::Detail {

    template <typename T>
    class Holder final : public Object {
      public:
        Holder(const char *kind, T v) : value(std::move(v)), kind_(kind) {}
        [[nodiscard]] const char *kind() const noexcept override { return kind_; }
        T value;

      private:
        const char *kind_;
    };

    template <typename T>
    ObjectPtr make_object(const char *kind, T v) {
        return std::make_shared<Holder<T>>(kind, std::move(v));
    }

    template <typename T>
    T &get(const ObjectPtr &object, const char *kind) {
        if (!object || std::strcmp(object->kind(), kind) != 0) {
            throw BindingError(IoErrorCode::InvalidArgument, std::string("expected a '") + kind + "' object" +
                                                                 (object ? std::string(", got '") + object->kind() + "'" : std::string(", got null")));
        }
        return static_cast<Holder<T> &>(*object).value;
    }

    template <typename>
    struct is_expected : std::false_type {};
    template <typename T, typename E>
    struct is_expected<std::expected<T, E>> : std::true_type {};

    /// Turns a finished handle into the Op's result. `convert(value, Value&)` (or `convert(Value&)` for void results).
    template <typename Handle, typename Convert>
    void settle(Op &op, Handle &handle, Convert &convert) {
        try {
            if constexpr (std::is_void_v<decltype(handle.wait())>) {
                handle.wait();
                Value v;
                convert(v);
                op.complete(std::move(v));
            } else {
                auto result = handle.wait();
                using R = std::decay_t<decltype(result)>;
                if constexpr (is_expected<R>::value) {
                    if (!result) {
                        op.fail(result.error());
                        return;
                    }
                    Value v;
                    if constexpr (std::is_void_v<typename R::value_type>) convert(v);
                    else convert(std::move(*result), v);
                    op.complete(std::move(v));
                } else {
                    Value v;
                    convert(std::move(result), v);
                    op.complete(std::move(v));
                }
            }
        } catch (const IoOperationCancelled &) {
            op.mark_cancelled();
        } catch (const ChannelClosed &error) {
            op.fail(IoErrorCode::Closed, error.what());
        } catch (const WatchClosed &error) {
            op.fail(IoErrorCode::Closed, error.what());
        } catch (const SemaphoreClosed &error) {
            op.fail(IoErrorCode::Closed, error.what());
        } catch (const TimeoutError &error) {
            op.fail(IoErrorCode::TimedOut, error.what());
        } catch (const std::exception &error) {
            op.fail(IoErrorCode::Unknown, error.what());
        } catch (...) {
            op.fail(IoErrorCode::Unknown, "unknown failure");
        }
    }

    /// Starts an operation: `start(token)` returns the handle; `convert` fills the Value when it finishes.
    template <typename Start, typename Convert>
    OpPtr launch(Start &&start, Convert convert) {
        auto op = std::make_shared<Op>();
        auto handle = start(op->token());
        handle.on_done([op, handle, convert = std::move(convert)]() mutable { settle(*op, handle, convert); });
        return op;
    }

    /// An operation that has already failed (synchronous argument problems that should still travel through the Op).
    inline OpPtr failed_op(IoErrorCode code, std::string message) {
        auto op = std::make_shared<Op>();
        op->fail(code, std::move(message));
        return op;
    }

    inline std::vector<std::byte> to_native(const Bytes &in) {
        std::vector<std::byte> out(in.size());
        if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
        return out;
    }
    inline Bytes from_native(const std::vector<std::byte> &in) {
        Bytes out(in.size());
        if (!in.empty()) std::memcpy(out.data(), in.data(), in.size());
        return out;
    }
    inline std::string to_string_bytes(const Bytes &in) { return std::string(reinterpret_cast<const char *>(in.data()), in.size()); }

    inline std::chrono::milliseconds ms(std::int64_t value) { return std::chrono::milliseconds(value < 0 ? 0 : value); }

    /// A held "sync_mutex" lock; keeps the mutex alive for as long as the guard lives.
    struct SyncGuardState {
        std::shared_ptr<Async::Mutex<Bytes>> mutex;
        std::optional<Async::MutexGuard<Bytes>> guard;
    };
    inline constexpr const char *kSyncGuard = "sync_guard";
    inline constexpr const char *kCookieJar = "cookie_jar";
    inline constexpr const char *kHttpServer = "http_server";
    inline constexpr const char *kRouter = "router";
    inline constexpr const char *kWebSocket = "websocket";

    inline std::vector<HttpHeader> to_headers(const Fields &fields) {
        std::vector<HttpHeader> out;
        out.reserve(fields.size());
        for (const auto &[name, value] : fields) out.push_back({name, value});
        return out;
    }
    inline Fields from_headers(const std::vector<HttpHeader> &headers) {
        Fields out;
        out.reserve(headers.size());
        for (const auto &h : headers) out.emplace_back(h.name, h.value);
        return out;
    }

    TlsClientOptions to_options(const TlsClientSpec &spec);
    TlsServerOptions to_options(const TlsServerSpec &spec);
    Fields tls_fields(const TlsInfo &info);

} // namespace SFT::Async::Binding::Detail
