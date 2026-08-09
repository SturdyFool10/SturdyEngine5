#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <Async/src/IoError.hpp>
#include <Async/src/Runtime.hpp>

namespace SFT::Async {

    using i64 = std::int64_t;
    using u16 = std::uint16_t;

    class TcpConnection;

    namespace Detail {
        struct TcpConnectionState;

        [[nodiscard]] std::expected<TcpConnection, IoError> connect_blocking(const std::string &host, u16 port);
        [[nodiscard]] std::expected<usize, IoError> send_blocking(
            const std::shared_ptr<TcpConnectionState> &state, std::span<const std::byte> data);
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> receive_blocking(
            const std::shared_ptr<TcpConnectionState> &state, usize max_bytes);
        [[nodiscard]] bool connection_is_open(const std::shared_ptr<TcpConnectionState> &state) noexcept;
        void close_blocking(const std::shared_ptr<TcpConnectionState> &state) noexcept;
    } // namespace Detail

    class TcpConnection {
      public:
        TcpConnection() noexcept = default;
        ~TcpConnection() noexcept;

        TcpConnection(const TcpConnection &) = delete;
        TcpConnection &operator=(const TcpConnection &) = delete;
        TcpConnection(TcpConnection &&other) noexcept;
        TcpConnection &operator=(TcpConnection &&other) noexcept;

        [[nodiscard]] bool is_open() const noexcept;
        void close() noexcept;

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] static auto connect(std::string host, u16 port) {
            return Rt::spawn([host = std::move(host), port]() {
                return Detail::connect_blocking(host, port);
            });
        }

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto send(std::vector<std::byte> data) {
            return Rt::spawn([state = state_, data = std::move(data)]() {
                return Detail::send_blocking(state, data);
            });
        }

        template <AsyncRuntime Rt = DefaultRuntime>
        [[nodiscard]] auto receive(usize max_bytes) {
            return Rt::spawn([state = state_, max_bytes]() {
                return Detail::receive_blocking(state, max_bytes);
            });
        }

      private:
        friend std::expected<TcpConnection, IoError> Detail::connect_blocking(const std::string &host, u16 port);

        explicit TcpConnection(i64 native_handle);

        std::shared_ptr<Detail::TcpConnectionState> state_;
    };

} // namespace SFT::Async
