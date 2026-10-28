// FileWatcher on both backends: native (inotify on Linux) and the portable polling one.
#include <Async/Async.hpp>
#include <Foundation/Log.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace SFT;
using namespace SFT::Async;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

    int failures = 0;
    const char *backend = "";

    void check(bool ok, const std::string &what) {
        if (!ok) {
            Foundation::log_error("FileWatchTest FAILED [{}]: {}", backend, what);
            ++failures;
        }
    }

    void write(const fs::path &path, const std::string &content, bool append = false) {
        std::ofstream out(path, append ? std::ios::app : std::ios::trunc);
        out << content;
    }

    /// Next event that satisfies `pred` within `limit`, skipping others.
    std::optional<FileEvent> wait_event(FileWatcher &watcher, const std::function<bool(const FileEvent &)> &pred, std::chrono::milliseconds limit = 3s) {
        CancellationSource stop;
        auto timer_stop = std::make_shared<CancellationSource>();
        auto timer = sleep_for(limit, timer_stop->token());
        timer.on_done([timer, &stop]() mutable {
            try {
                timer.wait();
                (void)stop.cancel();
            } catch (...) {
            }
        });
        std::optional<FileEvent> found;
        for (;;) {
            try {
                auto event = watcher.next(stop.token()).wait();
                if (!event) break;
                if (pred(*event)) {
                    found = std::move(*event);
                    break;
                }
            } catch (const IoOperationCancelled &) {
                break;
            }
        }
        (void)timer_stop->cancel();
        std::this_thread::sleep_for(5ms);
        return found;
    }

    bool quiet(FileWatcher &watcher, std::chrono::milliseconds window) {
        return !wait_event(watcher, [](const FileEvent &) { return true; }, window).has_value();
    }

    std::function<bool(const FileEvent &)> is(FileEventKind kind, const fs::path &path) {
        return [kind, path = path.string()](const FileEvent &e) { return e.kind == kind && e.path == path; };
    }

    void scenario(bool polling) {
        backend = polling ? "polling" : "native";
        const fs::path root = fs::temp_directory_path() / (polling ? "sturdy_watch_poll" : "sturdy_watch_native");
        fs::remove_all(root);
        fs::create_directories(root / "sub");

        FileWatchOptions options;
        options.force_polling = polling;
        options.poll_interval = 60ms;
        options.debounce = 40ms;
        options.recursive = true;
        auto made = FileWatcher::watch(root.string(), options);
        check(made.has_value(), "watch starts");
        if (!made) return;
        FileWatcher watcher = std::move(*made);
        check(watcher.uses_native_backend() == !polling, "the expected backend is in use");
        if (polling) std::this_thread::sleep_for(100ms); // let the first snapshot settle

        // create / modify / remove
        write(root / "a.txt", "one");
        check(wait_event(watcher, is(FileEventKind::Created, root / "a.txt")).has_value(), "Created is reported");
        std::this_thread::sleep_for(polling ? 1100ms : 80ms); // mtime granularity for the polling diff
        write(root / "a.txt", "one more", true);
        check(wait_event(watcher, is(FileEventKind::Modified, root / "a.txt")).has_value(), "Modified is reported");
        fs::remove(root / "a.txt");
        check(wait_event(watcher, is(FileEventKind::Removed, root / "a.txt")).has_value(), "Removed is reported");

        // recursion: a file in a pre-existing subdirectory, and in a directory created afterwards
        write(root / "sub" / "deep.txt", "x");
        check(wait_event(watcher, is(FileEventKind::Created, root / "sub" / "deep.txt")).has_value(), "recursive: file in an existing subdirectory");
        fs::create_directory(root / "later");
        check(wait_event(watcher, [&](const FileEvent &e) { return e.kind == FileEventKind::Created && e.path == (root / "later").string() && e.is_directory; }).has_value(),
              "a new directory is reported as a directory");
        std::this_thread::sleep_for(polling ? 150ms : 50ms);
        write(root / "later" / "inner.txt", "y");
        check(wait_event(watcher, is(FileEventKind::Created, root / "later" / "inner.txt")).has_value(), "recursive: file in a directory created after the watch started");

        if (!polling) {
            // rename pairs (native only; polling sees a removal and a creation)
            write(root / "old.txt", "r");
            (void)wait_event(watcher, is(FileEventKind::Created, root / "old.txt"));
            fs::rename(root / "old.txt", root / "new.txt");
            auto renamed = wait_event(watcher, [&](const FileEvent &e) { return e.kind == FileEventKind::Renamed; });
            check(renamed && renamed->path == (root / "new.txt").string() && renamed->old_path == (root / "old.txt").string(), "a rename is one event with both names");

            // a moved-in directory tree keeps being watched under its new name
            fs::rename(root / "later", root / "moved");
            (void)wait_event(watcher, [&](const FileEvent &e) { return e.kind == FileEventKind::Renamed; });
            write(root / "moved" / "after.txt", "z");
            check(wait_event(watcher, is(FileEventKind::Created, root / "moved" / "after.txt")).has_value(), "watches follow a renamed directory");
        }

        // bursts of writes are merged
        write(root / "burst.txt", "0");
        (void)wait_event(watcher, is(FileEventKind::Created, root / "burst.txt"));
        if (polling) std::this_thread::sleep_for(1100ms);
        (void)quiet(watcher, 150ms);
        for (int i = 0; i < 50; ++i) write(root / "burst.txt", std::to_string(i), true);
        int modified = 0;
        while (wait_event(watcher, is(FileEventKind::Modified, root / "burst.txt"), 400ms)) ++modified;
        check(modified >= 1 && modified <= 5, "50 rapid writes become a handful of Modified events, not 50 (got " + std::to_string(modified) + ")");

        // streams and closing
        auto stream_events = watcher.events().take(1).collect();
        write(root / "streamed.txt", "s");
        const auto got = stream_events.wait();
        check(got.size() == 1, "events() works as a Stream");

        auto pending = watcher.next();
        watcher.close();
        check(!pending.wait().has_value(), "close() fails a pending next()");
        check(!watcher.next().wait().has_value(), "next() after close fails");

        fs::remove_all(root);
    }

    void non_recursive_and_errors() {
        backend = "native";
        const fs::path root = fs::temp_directory_path() / "sturdy_watch_flat";
        fs::remove_all(root);
        fs::create_directories(root / "sub");
        FileWatchOptions options;
        options.recursive = false;
        options.debounce = 20ms;
        auto watcher = FileWatcher::watch(root.string(), options);
        check(watcher.has_value(), "flat watch starts");
        if (watcher) {
            write(root / "sub" / "hidden.txt", "x");
            write(root / "visible.txt", "x");
            check(wait_event(*watcher, is(FileEventKind::Created, root / "visible.txt")).has_value(), "top-level change is seen");
            check(!wait_event(*watcher, [](const FileEvent &e) { return e.path.find("hidden") != std::string::npos; }, 300ms).has_value(),
                  "changes below a subdirectory are not reported without recursive");
        }
        check(!FileWatcher::watch((root / "missing").string()).has_value(), "watching a missing path fails");

        // watching a single file
        write(root / "single.txt", "1");
        auto single = FileWatcher::watch((root / "single.txt").string(), options);
        check(single.has_value(), "a single file can be watched");
        if (single) {
            std::this_thread::sleep_for(50ms);
            write(root / "single.txt", "2", true);
            check(wait_event(*single, is(FileEventKind::Modified, root / "single.txt")).has_value(), "single-file watch reports modification");
        }

        // an overflowing queue reports it instead of growing
        FileWatchOptions small;
        small.max_pending_events = 4;
        small.debounce = 0ms;
        auto flood = FileWatcher::watch(root.string(), small);
        if (flood) {
            for (int i = 0; i < 50; ++i) write(root / ("f" + std::to_string(i)), "x");
            std::this_thread::sleep_for(150ms);
            bool overflow = false;
            for (int i = 0; i < 10 && !overflow; ++i) {
                auto e = wait_event(*flood, [](const FileEvent &) { return true; }, 200ms);
                overflow = e && e->kind == FileEventKind::Overflow;
            }
            check(overflow, "a full queue yields an Overflow event");
        }
        fs::remove_all(root);
    }

} // namespace

int main() {
    Scheduler::initialize(2);
    scenario(false);
    scenario(true);
    non_recursive_and_errors();
    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
