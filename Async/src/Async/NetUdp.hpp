#pragma once

// Private: UDP datagram transfers through io_uring, with Asio's reactor as the fallback. The same pair of calls serves the UDP
// socket object and the QUIC endpoint. Each function does exactly what the Asio call it replaces does (same handler signature,
// completion delivered on the handler's own executor), so callers change one line.

#include <Async/NetCore.hpp>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/uio.h>
#endif

namespace SFT::Async::Detail {

#if defined(__linux__)
    /// What one datagram operation needs to outlive its submission: the message header, the gather entry, the peer address.
    struct UdpRingOp {
        msghdr message{};
        iovec gather{};
        sockaddr_storage address{};
        udp::endpoint endpoint;
        std::vector<std::uint8_t> copy; ///< detached sends own their bytes
    };
#endif

    /// `socket.async_receive_from(buffer, sender, handler)`; `sender` must stay valid until the handler runs.
    template <typename Handler>
    void udp_receive_from(udp::socket &socket, asio::mutable_buffer buffer, udp::endpoint &sender, Handler &&handler) {
#if defined(__linux__)
        if (net_uring()) {
            auto boxed = std::make_shared<std::decay_t<Handler>>(std::forward<Handler>(handler));
            const auto executor = asio::get_associated_executor(*boxed, socket.get_executor());
            auto op = std::make_shared<UdpRingOp>();
            op->gather = iovec{buffer.data(), buffer.size()};
            op->message.msg_name = &op->address;
            op->message.msg_namelen = sizeof(op->address);
            op->message.msg_iov = &op->gather;
            op->message.msg_iovlen = 1;
            netring::recvmsg(socket.native_handle(), &op->message, [boxed, op, executor, &sender](int result) {
                ErrorCode ec;
                std::size_t transferred = 0;
                udp::endpoint from;
                if (result < 0) {
                    ec = ring_error(result);
                } else {
                    transferred = static_cast<std::size_t>(result);
                    const std::size_t length = (std::min<std::size_t>)(op->message.msg_namelen, from.capacity());
                    std::memcpy(from.data(), &op->address, length);
                    from.resize(length);
                }
                asio::dispatch(executor, [boxed, ec, transferred, from, &sender]() mutable {
                    if (!ec) sender = from;
                    (*boxed)(ec, transferred);
                });
            });
            return;
        }
#endif
        socket.async_receive_from(buffer, sender, std::forward<Handler>(handler));
    }

    /// `socket.async_send_to(buffer, to, handler)`; the bytes must stay valid until the handler runs.
    template <typename Handler>
    void udp_send_to(udp::socket &socket, asio::const_buffer buffer, const udp::endpoint &to, Handler &&handler) {
#if defined(__linux__)
        if (net_uring()) {
            auto boxed = std::make_shared<std::decay_t<Handler>>(std::forward<Handler>(handler));
            const auto executor = asio::get_associated_executor(*boxed, socket.get_executor());
            auto op = std::make_shared<UdpRingOp>();
            op->endpoint = to;
            op->gather = iovec{const_cast<void *>(buffer.data()), buffer.size()};
            op->message.msg_name = op->endpoint.data();
            op->message.msg_namelen = static_cast<socklen_t>(op->endpoint.size());
            op->message.msg_iov = &op->gather;
            op->message.msg_iovlen = 1;
            netring::sendmsg(socket.native_handle(), &op->message, [boxed, op, executor](int result) {
                ErrorCode ec;
                std::size_t transferred = 0;
                if (result < 0) ec = ring_error(result);
                else transferred = static_cast<std::size_t>(result);
                asio::dispatch(executor, [boxed, ec, transferred]() mutable { (*boxed)(ec, transferred); });
            });
            return;
        }
#endif
        socket.async_send_to(buffer, to, std::forward<Handler>(handler));
    }

    /// Fire-and-forget send: the datagram is copied, nothing reports back (a full buffer or an unreachable peer just drops it, which
    /// is what UDP means). The QUIC endpoint's transmit path.
    inline void udp_send_detached(udp::socket &socket, const std::uint8_t *data, std::size_t size, const udp::endpoint &to) {
#if defined(__linux__)
        if (net_uring()) {
            auto op = std::make_shared<UdpRingOp>();
            op->endpoint = to;
            op->copy.assign(data, data + size);
            op->gather = iovec{op->copy.data(), op->copy.size()};
            op->message.msg_name = op->endpoint.data();
            op->message.msg_namelen = static_cast<socklen_t>(op->endpoint.size());
            op->message.msg_iov = &op->gather;
            op->message.msg_iovlen = 1;
            netring::sendmsg(socket.native_handle(), &op->message, [op](int) {});
            return;
        }
#endif
        ErrorCode ignored;
        socket.send_to(asio::buffer(data, size), to, 0, ignored);
    }

} // namespace SFT::Async::Detail
