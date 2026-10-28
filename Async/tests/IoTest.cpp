// The unified byte-stream layer: copy, buffered readers/writers, framing, across sockets, files, processes and pipes.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("IoTest FAILED: {}", what);
            ++failures;
        }
    }

    template <typename T>
    bool failed_with(const std::expected<T, IoError> &result, IoErrorCode code) {
        return !result && result.error().code == code;
    }

    Io::Bytes pattern(std::size_t n) {
        Io::Bytes out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::byte>((i * 2654435761u) >> 13);
        return out;
    }

    TaskHandle<std::string> echo_lines(std::shared_ptr<TcpConnection> connection) {
        BufferedReader reader(*connection);
        std::string seen;
        for (;;) {
            auto line = co_await reader.read_line();
            if (!line || !*line) break;
            seen += **line + "|";
            (void)co_await connection->send_all(Io::to_bytes("echo:" + **line + "\n"));
        }
        co_return seen;
    }

    TaskHandle<void> serve_frames(std::shared_ptr<TcpConnection> connection, std::size_t* received) {
        Framed<TcpConnection, LengthDelimitedCodec> wire(*connection);
        for (;;) {
            auto frame = co_await wire.next();
            if (!frame || !*frame) break;
            ++*received;
            Io::Bytes reply = **frame;
            std::reverse(reply.begin(), reply.end());
            (void)co_await wire.send(reply);
        }
    }

} // namespace

int main() {
    Scheduler::initialize(2);

    // copy between in-memory ends, with a tiny pipe so back-pressure is exercised.
    {
        const auto data = pattern(1'000'000);
        auto [a, b] = memory_pipe(2);
        MemoryWriter sink;
        auto copy_in = [&]() -> TaskHandle<std::expected<std::uint64_t, IoError>> { return copy(*b, sink, {}, {}); };
        auto copying = copy_in();
        auto sending = [&]() -> TaskHandle<void> {
            for (std::size_t at = 0; at < data.size(); at += 50'000) {
                (void)co_await write_all(*a, Io::Bytes(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>((std::min)(at + 50'000, data.size()))));
            }
            (void)co_await IoTraits<PipeEnd>::shutdown(*a);
        }();
        sending.wait();
        auto copied = copying.wait();
        check(copied && *copied == data.size() && sink.bytes() == data, "copy moves 1 MB through a 2-chunk pipe intact (back-pressure, no loss)");
    }

    // copy limits and writer shutdown.
    {
        MemoryReader source(std::string_view("0123456789"));
        MemoryWriter sink;
        CopyOptions limited;
        limited.limit = 4;
        auto copied = copy(source, sink, limited).wait();
        check(copied && *copied == 4 && sink.text() == "0123", "copy honours its byte limit");
    }

    // file -> tcp -> file: the same algorithm for a different pair of types.
    {
        const fs::path dir = fs::temp_directory_path() / "sturdy_io_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const auto data = pattern(2'500'000);
        {
            std::ofstream out(dir / "in.bin", std::ios::binary);
            out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        auto listener = TcpListener::listen("127.0.0.1", 0).wait().value();
        auto accepting = listener.accept();
        auto client = TcpConnection::connect("127.0.0.1", listener.local_port().value(), 2s).wait().value();
        auto server = accepting.wait().value();

        auto source = File::open((dir / "in.bin").string(), OpenMode::Read).wait().value();
        auto target = File::open((dir / "out.bin").string(), OpenMode::Write | OpenMode::Create | OpenMode::Truncate).wait().value();
        CopyOptions to_wire;
        to_wire.shutdown_writer = true; // half-close tells the receiver the file is complete
        auto upload = copy(source, client, to_wire);
        auto download = copy(server, target);
        auto sent = upload.wait();
        auto received = download.wait();
        check(sent && *sent == data.size() && received && *received == data.size(), "a 2.5 MB file travels file -> socket -> file");
        (void)target.sync().wait();
        std::ifstream check_in(dir / "out.bin", std::ios::binary);
        const std::string round((std::istreambuf_iterator<char>(check_in)), {});
        check(round.size() == data.size() && std::memcmp(round.data(), data.data(), data.size()) == 0, "...and arrives byte-for-byte identical");
        fs::remove_all(dir);
    }

    // BufferedReader: lines over a socket, split across segments, with CRLF.
    {
        auto listener = TcpListener::listen("127.0.0.1", 0).wait().value();
        auto accepting = listener.accept();
        auto client = std::make_shared<TcpConnection>(TcpConnection::connect("127.0.0.1", listener.local_port().value(), 2s).wait().value());
        auto server = std::make_shared<TcpConnection>(accepting.wait().value());
        auto serving = echo_lines(server);
        (void)client->send_all(Io::to_bytes("alpha\r\nbe")).wait();
        std::this_thread::sleep_for(30ms);
        (void)client->send_all(Io::to_bytes("ta\ngamma")).wait();
        std::this_thread::sleep_for(30ms);
        (void)client->send_all(Io::to_bytes("\n")).wait();
        BufferedReader replies(*client);
        check(**replies.read_line().wait() == "echo:alpha", "lines are reassembled across segments (CRLF stripped)");
        check(**replies.read_line().wait() == "echo:beta", "second line");
        check(**replies.read_line().wait() == "echo:gamma", "third line, even though its terminator came in a separate segment");
        (void)client->shutdown_write().wait();
        check(serving.wait() == "alpha|beta|gamma|", "the server saw exactly the three lines and then EOF");
        (void)server->shutdown_write().wait(); // now the client can see the end of the conversation too
        check(!replies.read_line().wait()->has_value(), "end of stream reads as no more lines");
    }

    // BufferedReader limits and exact reads.
    {
        MemoryReader source(std::string_view("abcdefghij\nxyz"));
        BufferedReader reader(source, 4);
        check(failed_with(reader.read_until("\n", 5).wait(), IoErrorCode::LimitExceeded), "a record longer than the limit is refused");
        MemoryReader source2(std::string_view("hello world"));
        BufferedReader reader2(source2, 3);
        check(Io::to_string(reader2.read_exact(5).wait().value()) == "hello", "read_exact");
        check(Io::to_string(reader2.read_some(100).wait().value()).size() > 0, "read_some drains the buffer first");
        check(failed_with(reader2.read_exact(100).wait(), IoErrorCode::UnexpectedEof), "read_exact past the end is UnexpectedEof");
        MemoryReader tail(std::string_view("no newline at end"));
        BufferedReader tail_reader(tail);
        check(failed_with(tail_reader.read_line().wait(), IoErrorCode::UnexpectedEof), "a final unterminated line is reported, not silently dropped");
    }

    // BufferedWriter batches small writes.
    {
        MemoryWriter sink;
        BufferedWriter writer(sink, 16);
        for (int i = 0; i < 10; ++i) (void)writer.write(Io::to_bytes("ab")).wait();
        check(sink.bytes().size() < 20, "small writes are held back");
        (void)writer.shutdown().wait();
        check(sink.text() == "abababababababababab", "flush delivers everything in order");
        (void)writer.write(Io::to_bytes(std::string(100, 'z'))).wait();
        check(sink.bytes().size() == 120, "a write bigger than the buffer goes straight through");
    }

    // Framing: length-delimited over TCP, with a hostile length, and lines over a pipe.
    {
        auto listener = TcpListener::listen("127.0.0.1", 0).wait().value();
        auto accepting = listener.accept();
        auto client = TcpConnection::connect("127.0.0.1", listener.local_port().value(), 2s).wait().value();
        auto server = std::make_shared<TcpConnection>(accepting.wait().value());
        std::size_t received = 0;
        auto serving = serve_frames(server, &received);
        Framed<TcpConnection, LengthDelimitedCodec> wire(client);
        for (int i = 0; i < 20; ++i) (void)wire.send(Io::to_bytes("frame-" + std::to_string(i)));
        bool ok = true;
        for (int i = 0; i < 20; ++i) {
            auto reply = wire.next().wait();
            std::string expected = "frame-" + std::to_string(i);
            std::reverse(expected.begin(), expected.end());
            ok = ok && reply && *reply && Io::to_string(**reply) == expected;
        }
        check(ok, "20 pipelined frames round-trip in order");
        check(received == 20, "the server decoded every frame");

        // A length prefix claiming 2 GB makes the server's decoder stop at once (no allocation); closing its side then
        // gives the client a clean end of stream.
        Io::Bytes hostile = {std::byte{0x7F}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{1}};
        (void)client.send_all(hostile).wait();
        serving.wait();
        (void)server->shutdown_write().wait();
        auto tail = wire.next().wait();
        check(tail.has_value() && !tail->has_value(), "after the server rejected the hostile length, the client sees a clean end of stream");
    }
    {
        LengthDelimitedCodec small;
        small.max_frame_bytes = 8;
        Io::Bytes wire;
        check(failed_with(small.encode(Io::to_bytes("123456789"), wire), IoErrorCode::LimitExceeded), "the encoder enforces the frame limit too");
        Io::Bytes hostile = {std::byte{0x7F}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
        check(failed_with(small.decode(hostile), IoErrorCode::LimitExceeded), "a 2 GB length prefix is rejected by the decoder immediately");

        auto [a, b] = memory_pipe(4);
        Framed<PipeEnd, LinesCodec> left(*a), right(*b);
        (void)left.send("first").wait();
        (void)left.send("second").wait();
        (void)IoTraits<PipeEnd>::shutdown(*a).wait();
        auto all = right.frames().collect().wait();
        check((all == std::vector<std::string>{"first", "second"}), "Framed::frames() is an async Stream of decoded lines");
        check(failed_with(left.send("has\nnewline").wait(), IoErrorCode::InvalidArgument), "the lines encoder refuses embedded newlines");
    }

    // Processes through the same interface: cat is a copy pipeline.
    {
        ProcessOptions options;
        options.program = "cat";
        options.stdin_mode = StdioMode::Pipe;
        auto process = Process::spawn(options).value();
        ProcessStdin in{process};
        ProcessStdout out{process};
        const auto data = pattern(400'000);
        MemoryReader source(data);
        MemoryWriter sink;
        CopyOptions close_when_done;
        close_when_done.shutdown_writer = true;
        auto feed = copy(source, in, close_when_done);
        auto drain = copy(out, sink);
        check(feed.wait().has_value(), "copy into a child's stdin");
        auto drained = drain.wait();
        check(drained && *drained == data.size() && sink.bytes() == data, "copy out of a child's stdout: cat as a pass-through");
        auto status = process.wait().wait();
        check(status && status->success(), "the child exited cleanly once stdin was closed");
    }

    // read_to_end limits.
    {
        MemoryReader big(Io::Bytes(100'000));
        check(failed_with(read_to_end(big, 1000).wait(), IoErrorCode::LimitExceeded), "read_to_end refuses streams over its limit");
        MemoryReader fine(std::string_view("all of it"));
        check(Io::to_string(read_to_end(fine).wait().value()) == "all of it", "read_to_end reads everything");
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
