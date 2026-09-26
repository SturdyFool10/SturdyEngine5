/// TcpListener / UdpSocket loopback: accept + echo over TCP, datagram round trip over UDP, and close()
/// unblocking a pending accept.

#include <Async/Net.hpp>
#include <Async/Scheduler.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {
    using namespace SFT::Async;

    int failures = 0;
    void check(bool condition, const char *message) {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
            ++failures;
        }
    }

    std::vector<std::byte> bytes_of(const std::string &text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }
    std::string text_of(const std::vector<std::byte> &data) { return std::string(reinterpret_cast<const char *>(data.data()), data.size()); }
} // namespace

int main() {
    Scheduler::initialize(4);

    // TCP: listen on an OS-chosen port, connect, exchange bytes both ways.
    auto listener_result = TcpListener::listen("127.0.0.1", 0).wait();
    check(listener_result.has_value(), "listen on an ephemeral loopback port must succeed");
    if (!listener_result) {
        Scheduler::shutdown();
        return 1;
    }
    TcpListener listener = std::move(*listener_result);
    const auto port = listener.local_port();
    check(port.has_value() && *port != 0, "local_port must report the chosen port");

    auto accepted = listener.accept();
    auto client_result = TcpConnection::connect("127.0.0.1", *port).wait();
    check(client_result.has_value(), "connecting to the listener must succeed");
    auto server_result = accepted.wait();
    check(server_result.has_value(), "accept must return the incoming connection");
    if (client_result && server_result) {
        TcpConnection client = std::move(*client_result);
        TcpConnection server = std::move(*server_result);
        check(client.send(bytes_of("ping")).wait().has_value(), "client send");
        auto request = server.receive(16).wait();
        check(request.has_value() && text_of(*request) == "ping", "server must receive what the client sent");
        check(server.send(bytes_of("pong")).wait().has_value(), "server send");
        auto reply = client.receive(16).wait();
        check(reply.has_value() && text_of(*reply) == "pong", "client must receive the server's reply");
    }

    // A pending accept ends when the listener is closed.
    auto pending = listener.accept();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    listener.close();
    auto ended = pending.wait();
    check(!ended.has_value(), "closing the listener must end a pending accept with an error");
    check(!listener.is_open(), "a closed listener reports closed");

    // UDP: two sockets exchange a datagram; sender identity is reported.
    auto receiver_result = UdpSocket::bind("127.0.0.1", 0).wait();
    auto sender_result = UdpSocket::bind("127.0.0.1", 0).wait();
    check(receiver_result.has_value() && sender_result.has_value(), "binding UDP sockets must succeed");
    if (receiver_result && sender_result) {
        UdpSocket receiver = std::move(*receiver_result);
        UdpSocket sender = std::move(*sender_result);
        const auto receiver_port = receiver.local_port();
        const auto sender_port = sender.local_port();
        auto incoming = receiver.receive_from(64);
        check(sender.send_to("127.0.0.1", *receiver_port, bytes_of("datagram")).wait().has_value(), "send_to must succeed");
        auto datagram = incoming.wait();
        check(datagram.has_value() && text_of(datagram->data) == "datagram", "the datagram must arrive intact");
        check(datagram.has_value() && datagram->sender_port == *sender_port && datagram->sender_host == "127.0.0.1",
              "the datagram must report who sent it");

        auto blocked = receiver.receive_from(64);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        receiver.close();
        check(!blocked.wait().has_value(), "closing the socket must end a pending receive");
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
