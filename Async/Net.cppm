module;

#pragma region Imports
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>
#pragma endregion

export module Sturdy.Async:Net;

import Sturdy.Foundation;
import :Runtime;
import :IoError;

using std::expected;
using std::span;
using std::string;
using std::vector;

export namespace SFT::Async {

    class TcpConnection;

    namespace Detail {
        [[nodiscard]] expected<TcpConnection, IoError> connect_blocking(const string &host, u16 port);
        [[nodiscard]] expected<usize, IoError> send_blocking(i64 handle, span<const std::byte> data);
        [[nodiscard]] expected<vector<std::byte>, IoError> receive_blocking(i64 handle, usize max_bytes);
        void close_blocking(i64 handle) noexcept;
    } // namespace Detail

    // A connected TCP client socket. Like :File, "async" here means "runs the blocking syscall via
    // the runtime's task mechanism instead of on the calling thread" — not a non-blocking-socket /
    // event-loop reactor. This is a minimal client primitive (connect/send/receive), not a
    // networking stack: no listening/accept, no UDP, no TLS. Not available on Web at all — browsers
    // give wasm no raw TCP socket access (only fetch()/WebSocket, which are a different shape of
    // API entirely); wire that separately if/when it's needed.
    //
    // Move-only, and not thread-safe against itself: the caller must keep a `TcpConnection` alive
    // (and not call another operation on it concurrently) until any `send()`/`receive()` task
    // already spawned from it has completed.
    class TcpConnection {
      public:
        TcpConnection() noexcept = default;
        ~TcpConnection() noexcept;

        TcpConnection(const TcpConnection &) = delete;
        TcpConnection &operator=(const TcpConnection &) = delete;
        TcpConnection(TcpConnection &&other) noexcept;
        TcpConnection &operator=(TcpConnection &&other) noexcept;

        [[nodiscard]] bool is_open() const noexcept;

        // Closes the connection now, synchronously (closing a socket does not block meaningfully).
        // Safe to call on an already-closed connection. Also called automatically by the destructor.
        void close() noexcept;

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] static auto connect(string host, u16 port) {
            return Rt::spawn([host = std::move(host), port]() {
                return Detail::connect_blocking(host, port);
            });
        }

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto send(vector<std::byte> data) {
            return Rt::spawn([this, data = std::move(data)]() {
                return Detail::send_blocking(handle_, data);
            });
        }

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto receive(usize max_bytes) {
            return Rt::spawn([this, max_bytes]() {
                return Detail::receive_blocking(handle_, max_bytes);
            });
        }

      private:
        friend expected<TcpConnection, IoError> Detail::connect_blocking(const string &host, u16 port);

        explicit TcpConnection(i64 native_handle) noexcept
            : handle_(native_handle) {
        }

        i64 handle_ = -1;
    };

} // namespace SFT::Async
