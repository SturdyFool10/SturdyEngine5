#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Async/IoError.hpp>
#include <Async/Runtime.hpp>

namespace SFT::Async {

    using i64 = std::int64_t;
    using u16 = std::uint16_t;

    class TcpConnection;
    class TcpListener;
    class UdpSocket;

    /// One received UDP datagram and where it came from.
    struct UdpDatagram {
        std::vector<std::byte> data;
        std::string sender_host;
        u16 sender_port = 0;
    };

    namespace Detail {
        struct TcpConnectionState;

        [[nodiscard]] std::expected<TcpListener, IoError> listen_blocking(const std::string &host, u16 port, int backlog);
        [[nodiscard]] std::expected<TcpConnection, IoError> accept_blocking(const std::shared_ptr<TcpConnectionState> &state);
        [[nodiscard]] std::expected<UdpSocket, IoError> udp_bind_blocking(const std::string &host, u16 port);
        [[nodiscard]] std::expected<usize, IoError> udp_send_blocking(
            const std::shared_ptr<TcpConnectionState> &state, const std::string &host, u16 port, std::span<const std::byte> data);
        [[nodiscard]] std::expected<UdpDatagram, IoError> udp_receive_blocking(
            const std::shared_ptr<TcpConnectionState> &state, usize max_bytes);
        [[nodiscard]] std::expected<u16, IoError> local_port_blocking(const std::shared_ptr<TcpConnectionState> &state);

        /// Performs the connect blocking operation using the supplied arguments.
        ///
        /// @param host `host` value used by the operation.
        /// @param port `port` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::Unknown`, `IoErrorCode::NotFound`, `IoErrorCode::ConnectionRefused`.
        [[nodiscard]] std::expected<TcpConnection, IoError> connect_blocking(const std::string &host, u16 port);
        /// Performs the send blocking operation using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::InvalidArgument`, `IoErrorCode::ConnectionReset`.
        [[nodiscard]] std::expected<usize, IoError> send_blocking(
            const std::shared_ptr<TcpConnectionState> &state, std::span<const std::byte> data);
        /// Performs the receive blocking operation using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        /// @param max_bytes `max_bytes` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::InvalidArgument`, `IoErrorCode::Unknown`, `IoErrorCode::ConnectionReset`.
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> receive_blocking(
            const std::shared_ptr<TcpConnectionState> &state, usize max_bytes);
        /// Reports whether connection is open.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool connection_is_open(const std::shared_ptr<TcpConnectionState> &state) noexcept;
        /// Closes blocking using the supplied arguments and current state.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void close_blocking(const std::shared_ptr<TcpConnectionState> &state) noexcept;
    } // namespace Detail

    class TcpConnection {
      public:
        /// Constructs a `TcpConnection` in its default state.
        ///
        /// @note This function does not throw exceptions.
        TcpConnection() noexcept = default;
        /// Destroys the `TcpConnection` and releases resources owned by it.
        ///
        /// @note This function does not throw exceptions.
        ~TcpConnection() noexcept;

        /// Disables this construction form for `TcpConnection`.
        ///
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        TcpConnection(const TcpConnection &) = delete;
        /// Assigns a new value to this `TcpConnection`.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This overload is deleted; attempting to call it is a compile-time error.
        TcpConnection &operator=(const TcpConnection &) = delete;
        /// Constructs a `TcpConnection` from another instance.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @note This function does not throw exceptions.
        TcpConnection(TcpConnection &&other) noexcept;
        /// Assigns a new value to this `TcpConnection`.
        ///
        /// @param other Other object used by the operation.
        ///
        /// @return Returns `*this` so the operation can be chained.
        /// @note This function does not throw exceptions.
        TcpConnection &operator=(TcpConnection &&other) noexcept;

        /// Reports whether open holds for this `TcpConnection`.
        ///
        /// @return Returns `true` when the stated condition holds; otherwise returns `false`.
        /// @note This function does not throw exceptions.
        [[nodiscard]] bool is_open() const noexcept;
        /// Closes the supplied or associated value/state using the supplied arguments and current state.
        ///
        /// @note This function does not throw exceptions.
        void close() noexcept;

        /// Returns the current or globally available connect value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] static auto connect(std::string host, u16 port) {
            return Rt::spawn([host = std::move(host), port]() {
                return Detail::connect_blocking(host, port);
            });
        }

        /// Returns the current or globally available send value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto send(std::vector<std::byte> data) {
            return Rt::spawn([state = state_, data = std::move(data)]() {
                return Detail::send_blocking(state, data);
            });
        }

        /// Returns the current or globally available receive value.
        ///
        /// @return Returns the value produced by the operation.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto receive(usize max_bytes) {
            return Rt::spawn([state = state_, max_bytes]() {
                return Detail::receive_blocking(state, max_bytes);
            });
        }

      private:
        /// Performs the connect blocking operation for `TcpConnection` using the supplied arguments.
        ///
        /// @param host `host` value used by the operation.
        /// @param port `port` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        friend std::expected<TcpConnection, IoError> Detail::connect_blocking(const std::string &host, u16 port);
        friend std::expected<TcpConnection, IoError> Detail::accept_blocking(const std::shared_ptr<Detail::TcpConnectionState> &state);

        /// Constructs a `TcpConnection` from the supplied initialization values.
        ///
        /// @param native_handle Handle identifying the target object or resource.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit TcpConnection(i64 native_handle);

        std::shared_ptr<Detail::TcpConnectionState> state_;
    };

    /// A listening TCP socket: the server-side counterpart of `TcpConnection::connect`.
    ///
    /// `accept` waits in short slices and re-checks whether the listener was closed, so `close()` from
    /// another thread reliably ends a pending accept on every platform.
    class TcpListener {
      public:
        TcpListener() noexcept = default;
        ~TcpListener() noexcept;

        TcpListener(const TcpListener &) = delete;
        TcpListener &operator=(const TcpListener &) = delete;
        TcpListener(TcpListener &&other) noexcept;
        TcpListener &operator=(TcpListener &&other) noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        void close() noexcept;

        /// The port actually bound; useful after binding port 0 to let the OS choose.
        [[nodiscard]] std::expected<u16, IoError> local_port() const;

        /// Binds and listens on `host:port` (an empty host means all interfaces; port 0 picks a free one).
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] static auto listen(std::string host, u16 port, int backlog = 128) {
            return Rt::spawn([host = std::move(host), port, backlog]() { return Detail::listen_blocking(host, port, backlog); });
        }

        /// Waits for the next incoming connection.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto accept() {
            return Rt::spawn([state = state_]() { return Detail::accept_blocking(state); });
        }

      private:
        friend std::expected<TcpListener, IoError> Detail::listen_blocking(const std::string &host, u16 port, int backlog);

        explicit TcpListener(i64 native_handle);

        std::shared_ptr<Detail::TcpConnectionState> state_;
    };

    /// A bound UDP socket. Datagram boundaries are preserved: one `receive_from` returns one datagram.
    class UdpSocket {
      public:
        UdpSocket() noexcept = default;
        ~UdpSocket() noexcept;

        UdpSocket(const UdpSocket &) = delete;
        UdpSocket &operator=(const UdpSocket &) = delete;
        UdpSocket(UdpSocket &&other) noexcept;
        UdpSocket &operator=(UdpSocket &&other) noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        void close() noexcept;

        /// The port actually bound; useful after binding port 0.
        [[nodiscard]] std::expected<u16, IoError> local_port() const;

        /// Binds a UDP socket to `host:port` (an empty host means all interfaces; port 0 picks a free one).
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] static auto bind(std::string host, u16 port) {
            return Rt::spawn([host = std::move(host), port]() { return Detail::udp_bind_blocking(host, port); });
        }

        /// Sends one datagram to `host:port`; resolves to the number of bytes sent.
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto send_to(std::string host, u16 port, std::vector<std::byte> data) {
            return Rt::spawn([state = state_, host = std::move(host), port, data = std::move(data)]() {
                return Detail::udp_send_blocking(state, host, port, data);
            });
        }

        /// Waits for the next datagram of at most `max_bytes` bytes (longer datagrams are truncated).
        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto receive_from(usize max_bytes) {
            return Rt::spawn([state = state_, max_bytes]() { return Detail::udp_receive_blocking(state, max_bytes); });
        }

      private:
        friend std::expected<UdpSocket, IoError> Detail::udp_bind_blocking(const std::string &host, u16 port);

        explicit UdpSocket(i64 native_handle);

        std::shared_ptr<Detail::TcpConnectionState> state_;
    };

} // namespace SFT::Async
