#include <Foundation/Foundation.hpp>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <Async/Net.hpp>

using std::expected;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Async {

#if defined(_WIN32)
    namespace {

        // Windows needs one process-wide WSAStartup()/WSACleanup() pair; a function-local static
        // gives us the "call it exactly once, lazily, thread-safely" behavior for free. There is no
        // matching WSACleanup() call — the OS reclaims it at process exit, same tradeoff the rest
        // of the engine already makes for mimalloc/spdlog global state.
        bool ensure_winsock_initialized() noexcept {
            static const bool initialized = [] {
                WSADATA data;
                return WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }();
            return initialized;
        }

    } // namespace
#endif

    namespace Detail {

        namespace {

            // Both a POSIX fd (int, invalid == -1) and a Windows SOCKET (an unsigned, pointer-width
            // handle, invalid == all-bits-set) round-trip through i64 correctly: SOCKET's
            // INVALID_SOCKET (~0) reinterpreted as signed 64-bit is exactly -1, the same sentinel
            // POSIX already uses, so one `handle_ < 0` check covers "closed" on both platforms.
            void close_native(i64 handle) noexcept {
#if defined(_WIN32)
                closesocket(static_cast<SOCKET>(handle));
#else
                ::close(static_cast<int>(handle));
#endif
            }

        } // namespace

        expected<TcpConnection, IoError> connect_blocking(const string &host, u16 port) {
#if defined(_WIN32)
            if (!ensure_winsock_initialized()) {
                return unexpected(IoError{IoErrorCode::Unknown, "WSAStartup() failed"});
            }
#endif
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;

            addrinfo *resolved = nullptr;
            const string port_str = std::to_string(port);
            if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
                return unexpected(IoError{IoErrorCode::NotFound, "Failed to resolve host '" + host + "'"});
            }

            i64 handle = -1;
            for (addrinfo *candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
#if defined(_WIN32)
                const SOCKET native = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
                if (native == INVALID_SOCKET) {
                    continue;
                }
                if (::connect(native, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
                    handle = static_cast<i64>(native);
                    break;
                }
                closesocket(native);
#else
                const int native = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
                if (native < 0) {
                    continue;
                }
                if (::connect(native, candidate->ai_addr, candidate->ai_addrlen) == 0) {
                    handle = static_cast<i64>(native);
                    break;
                }
                ::close(native);
#endif
            }
            freeaddrinfo(resolved);

            if (handle < 0) {
                return unexpected(IoError{IoErrorCode::ConnectionRefused, "Failed to connect to '" + host + ":" + port_str + "'"});
            }
            return TcpConnection(handle);
        }

        expected<usize, IoError> send_blocking(i64 handle, span<const std::byte> data) {
            if (handle < 0) {
                return unexpected(IoError{IoErrorCode::InvalidArgument, "send() called on a closed TcpConnection"});
            }
#if defined(_WIN32)
            const int sent = ::send(static_cast<SOCKET>(handle), reinterpret_cast<const char *>(data.data()), static_cast<int>(data.size()), 0);
            if (sent == SOCKET_ERROR) {
                return unexpected(IoError{IoErrorCode::ConnectionReset, "send() failed"});
            }
#else
            const ssize_t sent = ::send(static_cast<int>(handle), data.data(), data.size(), 0);
            if (sent < 0) {
                return unexpected(IoError{IoErrorCode::ConnectionReset, "send() failed"});
            }
#endif
            return static_cast<usize>(sent);
        }

        expected<vector<std::byte>, IoError> receive_blocking(i64 handle, usize max_bytes) {
            if (handle < 0) {
                return unexpected(IoError{IoErrorCode::InvalidArgument, "receive() called on a closed TcpConnection"});
            }
            vector<std::byte> buffer(max_bytes);
#if defined(_WIN32)
            const int received = ::recv(static_cast<SOCKET>(handle), reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (received == SOCKET_ERROR) {
                return unexpected(IoError{IoErrorCode::ConnectionReset, "recv() failed"});
            }
#else
            const ssize_t received = ::recv(static_cast<int>(handle), buffer.data(), buffer.size(), 0);
            if (received < 0) {
                return unexpected(IoError{IoErrorCode::ConnectionReset, "recv() failed"});
            }
#endif
            buffer.resize(static_cast<usize>(received));
            return buffer;
        }

        void close_blocking(i64 handle) noexcept {
            if (handle >= 0) {
                close_native(handle);
            }
        }

    } // namespace Detail

    TcpConnection::~TcpConnection() noexcept {
        if (is_open()) {
            close();
        }
    }

    TcpConnection::TcpConnection(TcpConnection &&other) noexcept
        : handle_(other.handle_) {
        other.handle_ = -1;
    }

    TcpConnection &TcpConnection::operator=(TcpConnection &&other) noexcept {
        if (this != &other) {
            if (is_open()) {
                close();
            }
            handle_ = other.handle_;
            other.handle_ = -1;
        }
        return *this;
    }

    bool TcpConnection::is_open() const noexcept {
        return handle_ >= 0;
    }

    void TcpConnection::close() noexcept {
        Detail::close_blocking(handle_);
        handle_ = -1;
    }

} // namespace SFT::Async
