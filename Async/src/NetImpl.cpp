#include <Foundation/src/Foundation.hpp>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
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
#include <Async/src/Net.hpp>

using std::expected;
using std::span;
using std::string;
using std::unexpected;
using std::vector;

namespace SFT::Async {

#if defined(_WIN32)
    namespace {


        /// Finds or creates the winsock initialized required by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
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

        struct TcpConnectionState {
            /// Constructs a `TcpConnectionState` from the supplied initialization values.
            ///
            /// @param native_handle Handle identifying the target object or resource.
            ///
            /// @note This function does not throw exceptions.
            explicit TcpConnectionState(i64 native_handle) noexcept
                : handle(native_handle) {}

            std::mutex mutex;
            i64 handle = -1;
            usize active_operations = 0;
            bool closing = false;
        };

        namespace {


            /// Closes native using the supplied arguments and current state.
            ///
            /// @param handle Handle identifying the target object or resource.
            ///
            /// @note This function does not throw exceptions.
            void close_native(i64 handle) noexcept {
#if defined(_WIN32)
                closesocket(static_cast<SOCKET>(handle));
#else
                ::close(static_cast<int>(handle));
#endif
            }


            /// Shuts down native and releases associated runtime state.
            ///
            /// @param handle Handle identifying the target object or resource.
            ///
            /// @note This function does not throw exceptions.
            void shutdown_native(i64 handle) noexcept {
#if defined(_WIN32)
                ::shutdown(static_cast<SOCKET>(handle), SD_BOTH);
#else
                ::shutdown(static_cast<int>(handle), SHUT_RDWR);
#endif
            }

            class ConnectionOperation {
              public:
                /// Constructs a `ConnectionOperation` from the supplied initialization values.
                ///
                /// @param state `state` value used by the operation.
                ///
                /// @note This function does not throw exceptions.
                explicit ConnectionOperation(const std::shared_ptr<TcpConnectionState> &state) noexcept
                    : state_(state) {
                    if (!state_) {
                        return;
                    }
                    try {
                        std::lock_guard<std::mutex> lock(state_->mutex);
                        if (!state_->closing && state_->handle >= 0) {
                            handle_ = state_->handle;
                            ++state_->active_operations;
                        }
                    } catch (...) {
                        state_.reset();
                    }
                }

                /// Disables this construction form for `ConnectionOperation`.
                ///
                /// @note This overload is deleted; attempting to call it is a compile-time error.
                ConnectionOperation(const ConnectionOperation &) = delete;
                /// Assigns a new value to this `ConnectionOperation`.
                ///
                /// @return Returns `*this` so the operation can be chained.
                /// @note This overload is deleted; attempting to call it is a compile-time error.
                ConnectionOperation &operator=(const ConnectionOperation &) = delete;

                /// Destroys the `ConnectionOperation` and releases resources owned by it.
                ///
                /// @note This function does not throw exceptions.
                ~ConnectionOperation() noexcept {
                    if (!state_ || handle_ < 0) {
                        return;
                    }

                    i64 handle_to_close = -1;
                    try {
                        std::lock_guard<std::mutex> lock(state_->mutex);
                        --state_->active_operations;
                        if (state_->closing && state_->active_operations == 0 && state_->handle >= 0) {
                            handle_to_close = state_->handle;
                            state_->handle = -1;
                        }
                    } catch (...) {
                        return;
                    }
                    if (handle_to_close >= 0) {
                        close_native(handle_to_close);
                    }
                }

                /// Returns the current or globally available handle value.
                ///
                /// @return Returns the current handle value.
                /// @note This function does not throw exceptions.
                [[nodiscard]] i64 handle() const noexcept { return handle_; }

              private:
                std::shared_ptr<TcpConnectionState> state_;
                i64 handle_ = -1;
            };

            /// Performs the socket call size supported operation for `Detail` using the supplied arguments.
            ///
            /// @param size Requested or available size for the operation.
            ///
            /// @return Returns the boolean result of the operation.
            /// @note This function does not throw exceptions.
            [[nodiscard]] bool socket_call_size_supported(usize size) noexcept {
#if defined(_WIN32)
                return size <= static_cast<usize>((std::numeric_limits<int>::max)());
#else
                return size <= static_cast<usize>((std::numeric_limits<ssize_t>::max)());
#endif
            }

        } // namespace

        /// Performs the connect blocking operation for `Detail` using the supplied arguments.
        ///
        /// @param host `host` value used by the operation.
        /// @param port `port` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::Unknown`, `IoErrorCode::NotFound`, `IoErrorCode::ConnectionRefused`.
        expected<TcpConnection, IoError> connect_blocking(const string &host, u16 port) {
#if defined(_WIN32)
            if (!ensure_winsock_initialized()) {
                return unexpected(IoError{IoErrorCode::Unknown, "WSAStartup() failed"});
            }
#endif
            try {
                addrinfo hints{};
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                hints.ai_protocol = IPPROTO_TCP;

                addrinfo *resolved = nullptr;
                const string port_str = std::to_string(port);
                const int resolve_result = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &resolved);
                if (resolve_result != 0 || resolved == nullptr) {
                    if (resolved != nullptr) {
                        freeaddrinfo(resolved);
                    }
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

                try {
                    return TcpConnection(handle);
                } catch (...) {
                    close_native(handle);
                    return unexpected(IoError{IoErrorCode::Unknown, "Failed to create TcpConnection state"});
                }
            } catch (...) {
                return unexpected(IoError{IoErrorCode::Unknown, "Unexpected failure while connecting TcpConnection"});
            }
        }

        /// Performs the send blocking operation for `Detail` using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        /// @param data Data consumed or referenced by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::InvalidArgument`, `IoErrorCode::ConnectionReset`.
        expected<usize, IoError> send_blocking(const std::shared_ptr<TcpConnectionState> &state,
                                               span<const std::byte> data) {
            if (!socket_call_size_supported(data.size())) {
                return unexpected(IoError{IoErrorCode::InvalidArgument, "send() payload exceeds the platform socket-call limit"});
            }

            ConnectionOperation operation{state};
            const i64 handle = operation.handle();
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

        /// Performs the receive blocking operation for `Detail` using the supplied arguments.
        ///
        /// @param state `state` value used by the operation.
        /// @param max_bytes `max_bytes` value used by the operation.
        ///
        /// @return Returns the value alternative on success; the error alternative describes why the operation failed.
        /// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
        /// @note Error/status alternatives explicitly produced by this implementation include `IoErrorCode::InvalidArgument`, `IoErrorCode::Unknown`, `IoErrorCode::ConnectionReset`.
        expected<vector<std::byte>, IoError> receive_blocking(const std::shared_ptr<TcpConnectionState> &state,
                                                               usize max_bytes) {
            if (!socket_call_size_supported(max_bytes)) {
                return unexpected(IoError{IoErrorCode::InvalidArgument, "receive() limit exceeds the platform socket-call limit"});
            }

            ConnectionOperation operation{state};
            const i64 handle = operation.handle();
            if (handle < 0) {
                return unexpected(IoError{IoErrorCode::InvalidArgument, "receive() called on a closed TcpConnection"});
            }

            vector<std::byte> buffer;
            try {
                buffer.resize(max_bytes);
            } catch (...) {
                return unexpected(IoError{IoErrorCode::Unknown, "Failed to allocate receive buffer"});
            }
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

        /// Reports whether connection is open.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @return Returns the boolean result of the operation.
        /// @note This function does not throw exceptions.
        bool connection_is_open(const std::shared_ptr<TcpConnectionState> &state) noexcept {
            if (!state) {
                return false;
            }
            try {
                std::lock_guard<std::mutex> lock(state->mutex);
                return !state->closing && state->handle >= 0;
            } catch (...) {
                return false;
            }
        }

        /// Closes blocking using the supplied arguments and current state.
        ///
        /// @param state `state` value used by the operation.
        ///
        /// @note This function does not throw exceptions.
        void close_blocking(const std::shared_ptr<TcpConnectionState> &state) noexcept {
            if (!state) {
                return;
            }

            i64 handle = -1;
            bool close_now = false;
            try {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->closing) {
                    return;
                }
                state->closing = true;
                handle = state->handle;
                if (state->active_operations == 0) {
                    state->handle = -1;
                    close_now = true;
                }
            } catch (...) {
                return;
            }

            if (handle < 0) {
                return;
            }
            if (close_now) {
                close_native(handle);
            } else {
                shutdown_native(handle);
            }
        }

    } // namespace Detail

    /// Performs the TCP connection operation for `Async` using the supplied arguments.
    ///
    /// @param native_handle Handle identifying the target object or resource.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    TcpConnection::TcpConnection(i64 native_handle)
        : state_(std::make_shared<Detail::TcpConnectionState>(native_handle)) {}

    /// Destroys the `Async` and releases resources owned by it.
    ///
    /// @note This function does not throw exceptions.
    TcpConnection::~TcpConnection() noexcept {
        close();
    }

    /// Performs the TCP connection operation for `Async` using the supplied arguments.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @note This function does not throw exceptions.
    TcpConnection::TcpConnection(TcpConnection &&other) noexcept
        : state_(std::move(other.state_)) {}

    /// Assigns a new value to this `Async`.
    ///
    /// @param other Other object used by the operation.
    ///
    /// @return Returns `*this` so the operation can be chained.
    /// @note This function does not throw exceptions.
    TcpConnection &TcpConnection::operator=(TcpConnection &&other) noexcept {
        if (this != &other) {
            close();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /// Reports whether open holds for this `Async`.
    ///
    /// @return Returns the current is open value.
    /// @note This function does not throw exceptions.
    bool TcpConnection::is_open() const noexcept {
        return Detail::connection_is_open(state_);
    }

    /// Closes the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @return Returns the current close value.
    /// @note This function does not throw exceptions.
    void TcpConnection::close() noexcept {
        Detail::close_blocking(state_);
    }

} // namespace SFT::Async
