#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
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

    namespace Detail {
        [[nodiscard]] std::expected<TcpConnection, IoError> connect_blocking(const std::string &host, u16 port);
        [[nodiscard]] std::expected<usize, IoError> send_blocking(i64 handle, std::span<const std::byte> data);
        [[nodiscard]] std::expected<std::vector<std::byte>, IoError> receive_blocking(i64 handle, usize max_bytes);
        void close_blocking(i64 handle) noexcept;
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
        friend std::expected<TcpConnection, IoError> Detail::connect_blocking(const std::string &host, u16 port);

        explicit TcpConnection(i64 native_handle) noexcept
            : handle_(native_handle) {}

        i64 handle_ = -1;
    };

} // namespace SFT::Async
