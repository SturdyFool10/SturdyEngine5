// Streaming File handle plus the TCP stream helpers (send_all, receive_exact, half-close, addresses, timeouts).
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("IoStreamTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> bytes(std::string_view text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }

    std::string text(const std::vector<std::byte> &data) {
        return std::string(reinterpret_cast<const char *>(data.data()), data.size());
    }

    template <typename T>
    bool failed_with(const std::expected<T, IoError> &result, IoErrorCode code) {
        return !result && result.error().code == code;
    }

    void file_tests(const std::filesystem::path &dir) {
        const std::string path = (dir / "stream.bin").string();

        auto opened = File::open(path, OpenMode::Write | OpenMode::Create | OpenMode::Truncate | OpenMode::Read).wait();
        check(opened.has_value(), "open for create");
        if (!opened) {
            return;
        }
        File file = std::move(*opened);
        check(file.write(bytes("hello world")).wait().value() == 11, "write returns byte count");
        check(file.size().wait().value() == 11, "size after write");
        check(file.write_at(6, bytes("WORLD")).wait().value() == 5, "write_at");
        check(text(file.read_at(0, 64).wait().value()) == "hello WORLD", "read_at sees positional write, short at EOF");
        check(file.seek(0).wait().value() == 0, "seek to start");
        check(text(file.read(5).wait().value()) == "hello", "read advances cursor");
        check(text(file.read(1).wait().value()) == " ", "read continues from cursor");
        check(file.seek(-5, SeekOrigin::End).wait().value() == 6, "seek from end");
        check(text(file.read_to_end().wait().value()) == "WORLD", "read_to_end");
        check(file.read(10).wait().value().empty(), "read at EOF is empty");
        (void)file.seek(0).wait();
        check(failed_with(file.read_to_end(4).wait(), IoErrorCode::InvalidArgument), "read_to_end enforces its limit");
        check(file.set_len(4).wait().has_value() && file.size().wait().value() == 4, "set_len truncates");
        check(file.sync().wait().has_value() && file.sync(true).wait().has_value(), "sync");

        CancellationSource source;
        (void)source.cancel();
        bool cancelled = false;
        try {
            (void)file.read(10, source.token()).wait();
        } catch (const IoOperationCancelled &) {
            cancelled = true;
        }
        check(cancelled, "cancelled read throws");

        file.close();
        check(!file.is_open(), "closed");
        check(failed_with(file.read(1).wait(), IoErrorCode::InvalidArgument), "read after close fails");

        auto again = File::open(path, OpenMode::Write | OpenMode::CreateNew).wait();
        check(failed_with(again, IoErrorCode::AlreadyExists), "CreateNew refuses an existing file");
        auto missing = File::open((dir / "nope.bin").string(), OpenMode::Read).wait();
        check(failed_with(missing, IoErrorCode::NotFound), "open missing file");
        auto invalid = File::open(path, OpenMode::Create).wait();
        check(failed_with(invalid, IoErrorCode::InvalidArgument), "open without access mode");

        auto appender = File::open(path, OpenMode::Append).wait();
        check(appender.has_value(), "open append");
        if (appender) {
            check(appender->write(bytes("++")).wait().has_value(), "append write");
            check(failed_with(appender->write_at(0, bytes("x")).wait(), IoErrorCode::InvalidArgument), "write_at rejected in append");
            check(failed_with(appender->read(1).wait(), IoErrorCode::InvalidArgument), "append-only file is not readable");
        }
        check(text(read_file(path).wait().value()) == "hell++", "append landed at the end");

        // A large write goes through the chunk loop.
        std::vector<std::byte> big(5 * 1024 * 1024 + 123);
        for (std::size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<std::byte>(i * 31 + 7);
        }
        auto big_file = File::open(path, OpenMode::Write | OpenMode::Read | OpenMode::Truncate).wait();
        check(big_file.has_value(), "open big");
        if (big_file) {
            check(big_file->write(big).wait().value() == big.size(), "big write");
            (void)big_file->seek(0).wait();
            check(big_file->read_to_end().wait().value() == big, "big round trip");
        }
    }

    void net_tests() {
        auto listener = TcpListener::listen("127.0.0.1", 0).wait();
        check(listener.has_value(), "listen");
        if (!listener) {
            return;
        }
        const u16 port = listener->local_port().value();

        auto accepted = listener->accept();
        auto client_result = TcpConnection::connect("127.0.0.1", port, 2s).wait();
        check(client_result.has_value(), "connect with timeout");
        auto server_result = accepted.wait();
        check(server_result.has_value(), "accept");
        if (!client_result || !server_result) {
            return;
        }
        TcpConnection client = std::move(*client_result);
        TcpConnection server = std::move(*server_result);

        check(client.peer_address().value().port == port, "peer_address port");
        check(client.peer_address().value().host == "127.0.0.1", "peer_address host");
        check(server.peer_address().value().port == client.local_address().value().port, "addresses agree across the pair");
        check(client.set_nodelay(true).wait().has_value(), "set_nodelay");

        // 4 MiB through send_all/receive_exact: forces partial reads and writes on both sides.
        std::vector<std::byte> payload(4 * 1024 * 1024);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::byte>(i ^ (i >> 8));
        }
        auto sending = client.send_all(payload);
        auto receiving = server.receive_exact(payload.size());
        check(sending.wait().has_value(), "send_all completes");
        check(receiving.wait().value() == payload, "receive_exact returns every byte in order");

        // Half-close: server sees EOF, then can still answer.
        (void)client.send_all(bytes("bye")).wait();
        check(client.shutdown_write().wait().has_value(), "shutdown_write");
        check(text(server.receive_exact(3).wait().value()) == "bye", "data before half-close arrives");
        check(server.receive(16).wait().value().empty(), "half-close reads as EOF");
        check(server.send_all(bytes("ack")).wait().has_value(), "reply after peer half-close");
        check(text(client.receive_exact(3).wait().value()) == "ack", "client still reads after its own half-close");

        // Peer closes mid-read.
        (void)server.send_all(bytes("ab")).wait();
        server.close();
        check(failed_with(client.receive_exact(10).wait(), IoErrorCode::UnexpectedEof), "receive_exact reports UnexpectedEof");

        // Writing to a closed peer must come back as an error, not raise SIGPIPE (which would end this process).
        bool saw_error = false;
        for (int i = 0; i < 100 && !saw_error; ++i) {
            saw_error = !client.send_all(std::vector<std::byte>(64 * 1024)).wait().has_value();
            std::this_thread::sleep_for(2ms);
        }
        check(saw_error, "send to a closed peer reports an error instead of SIGPIPE");

        auto resolved = TcpConnection::resolve("localhost", 80).wait();
        check(resolved.has_value() && !resolved->empty(), "resolve localhost");
        check(failed_with(TcpConnection::resolve("no-such-host.invalid", 80).wait(), IoErrorCode::NotFound), "resolve failure");

        // Nothing listens on a port we just released.
        listener->close();
        auto refused = TcpConnection::connect("127.0.0.1", port, 500ms).wait();
        check(failed_with(refused, IoErrorCode::ConnectionRefused), "refused connect is reported");
    }

} // namespace

int main() {
    Scheduler::initialize(2);

    const auto dir = std::filesystem::temp_directory_path() / "sturdy_iostream_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    file_tests(dir);
    net_tests();

    std::filesystem::remove_all(dir);
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
