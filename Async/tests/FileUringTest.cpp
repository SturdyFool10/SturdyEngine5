// Positional file IO through io_uring (Linux) and its fallback to the IO lane: same results either way.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <filesystem>
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
            Foundation::log_error("FileUringTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> pattern(std::size_t n, unsigned seed) {
        std::vector<std::byte> out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::byte>(((i + seed) * 2654435761u) >> 13);
        return out;
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    const fs::path root = fs::temp_directory_path() / "sturdy_file_uring_test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string path = (root / "data.bin").string();

    const bool ring = Detail::file_uring_available();
    Foundation::log_info("FileUringTest: io_uring {}", ring ? "active" : "unavailable (testing the fallback)");

    auto opened = File::open(path, OpenMode::Read | OpenMode::Write | OpenMode::Create | OpenMode::Truncate).wait();
    check(opened.has_value(), "open");
    File file = std::move(*opened);

    // Positional write then read back, including a block that straddles nothing special and one at a distance.
    const auto block = pattern(100'000, 1);
    auto put = file.write_at(0, block).wait();
    check(put && *put == block.size(), "write_at reports the byte count");
    const auto far_block = pattern(5000, 7);
    check(file.write_at(1'000'000, far_block).wait().has_value(), "write_at far past the end extends the file");
    auto size = file.size().wait();
    check(size && *size == 1'005'000, "file size after sparse write");

    auto back = file.read_at(0, block.size()).wait();
    check(back && *back == block, "read_at returns what write_at stored");
    auto hole = file.read_at(500'000, 64).wait();
    check(hole && hole->size() == 64 && std::all_of(hole->begin(), hole->end(), [](std::byte b) { return b == std::byte{0}; }), "the hole reads as zeros");
    auto tail = file.read_at(1'004'000, 4096).wait();
    check(tail && tail->size() == 1000 && std::equal(tail->begin(), tail->end(), far_block.begin() + 4000), "a read crossing EOF is short");
    auto past = file.read_at(2'000'000, 10).wait();
    check(past && past->empty(), "a read past EOF is empty");

    // Many reads in flight at once all get their own bytes.
    std::vector<TaskHandle<std::expected<std::vector<std::byte>, IoError>>> pending;
    for (unsigned i = 0; i < 200; ++i) pending.push_back(file.read_at(i * 400, 400));
    bool all_ok = true;
    for (unsigned i = 0; i < pending.size(); ++i) {
        auto got = pending[i].wait();
        all_ok = all_ok && got && got->size() == 400 && std::equal(got->begin(), got->end(), block.begin() + i * 400);
    }
    check(all_ok, "200 concurrent read_at calls return the right bytes");

    // Concurrent writers to disjoint regions.
    std::vector<TaskHandle<std::expected<std::size_t, IoError>>> writes;
    for (unsigned i = 0; i < 64; ++i) writes.push_back(file.write_at(2'000'000 + i * 1024, pattern(1024, 100 + i)));
    bool writes_ok = true;
    for (auto &w : writes) {
        auto r = w.wait();
        writes_ok = writes_ok && r && *r == 1024;
    }
    check(writes_ok, "64 concurrent write_at calls succeed");
    auto verify = file.read_at(2'000'000, 64 * 1024).wait();
    bool verify_ok = verify && verify->size() == 64 * 1024;
    for (unsigned i = 0; verify_ok && i < 64; ++i) {
        const auto expected = pattern(1024, 100 + i);
        verify_ok = std::equal(expected.begin(), expected.end(), verify->begin() + i * 1024);
    }
    check(verify_ok, "the disjoint writes landed where they belong");

    check(file.sync().wait().has_value(), "sync");
    check(file.sync(true).wait().has_value(), "sync (data only)");

    // A big read (over the ring's 4 MiB cap) still works through the lane.
    auto huge = file.read_at(0, 8 * 1024 * 1024).wait();
    check(huge && huge->size() == 2'000'000 + 64 * 1024, "an 8 MiB request is served (via the lane)");

    // The cursor is unaffected by positional calls.
    auto cursor_read = file.read(10).wait();
    check(cursor_read && *cursor_read == std::vector<std::byte>(block.begin(), block.begin() + 10), "positional IO leaves the cursor alone");

    // Continuations run off any worker thread: awaiting in a coroutine works.
    auto coroutine = [&]() -> TaskHandle<bool> {
        auto bytes = co_await file.read_at(0, 16);
        co_return bytes && bytes->size() == 16;
    }();
    check(coroutine.wait(), "co_await on read_at");

    // Permissions: reading a write-only file and writing a read-only file are refused, ring or not.
    {
        auto wo = File::open((root / "wo.bin").string(), OpenMode::Write | OpenMode::Create).wait();
        check(wo.has_value(), "open write-only");
        auto r = wo->read_at(0, 10).wait();
        check(!r.has_value(), "read_at on a write-only file fails");
        auto ro = File::open(path, OpenMode::Read).wait();
        check(ro.has_value(), "open read-only");
        auto w = ro->write_at(0, pattern(10, 0)).wait();
        check(!w.has_value(), "write_at on a read-only file fails");
    }

    // After close(), operations fail cleanly instead of touching a stale descriptor.
    file.close();
    check(!file.read_at(0, 10).wait().has_value(), "read_at after close fails");
    check(!file.write_at(0, pattern(10, 0)).wait().has_value(), "write_at after close fails");
    check(!file.sync().wait().has_value(), "sync after close fails");

    fs::remove_all(root);
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
