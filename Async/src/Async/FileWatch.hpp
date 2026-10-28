#pragma once

// File-system watching (tokio-notify style): hot reload, asset pipelines, config reload, "tail the log directory".
//
// Linux uses inotify (event-driven, no polling). Every platform has the polling backend, which compares directory
// snapshots on the IO lane at `poll_interval`; Windows and macOS currently always use it (their native APIs --
// ReadDirectoryChangesW / FSEvents -- are not wired yet), so latency there is the poll interval and a rename shows up as
// Removed + Created.

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>

#include <Async/Cancellation.hpp>
#include <Async/IoError.hpp>
#include <Async/Stream.hpp>
#include <Async/Task.hpp>

namespace SFT::Async {

    enum class FileEventKind {
        Created,
        Modified,  ///< content or attributes changed (bursts of writes are coalesced, see `debounce`)
        Removed,
        Renamed,   ///< `path` is the new name, `old_path` the previous one (native backend only)
        Overflow,  ///< events were lost (queue full or kernel buffer overrun): rescan what you care about
    };

    struct FileEvent {
        FileEventKind kind = FileEventKind::Modified;
        std::string path;
        std::string old_path;
        bool is_directory = false;
    };

    struct FileWatchOptions {
        /// Also watch everything below a watched directory, including directories created later.
        bool recursive = false;
        /// Consecutive `Modified` events for one path closer together than this are merged into one.
        std::chrono::milliseconds debounce{50};
        /// Use the polling backend even where a native one exists (portable behaviour, testing).
        bool force_polling = false;
        std::chrono::milliseconds poll_interval{250};
        /// Undelivered events kept; beyond this one `Overflow` event is queued and the rest dropped.
        std::size_t max_pending_events = 4096;
    };

    namespace Detail {
        struct WatchCore;
    }

    /// Watches a file or directory. Move-only; destroying it stops the watch.
    class FileWatcher {
      public:
        FileWatcher() noexcept = default;
        ~FileWatcher() noexcept;
        FileWatcher(const FileWatcher &) = delete;
        FileWatcher &operator=(const FileWatcher &) = delete;
        FileWatcher(FileWatcher &&) noexcept = default;
        FileWatcher &operator=(FileWatcher &&other) noexcept;

        /// Starts watching `path`. Fails if it does not exist or the OS refuses (e.g. inotify limits).
        [[nodiscard]] static std::expected<FileWatcher, IoError> watch(const std::string &path, FileWatchOptions options = {});

        /// The next event, in the order the changes happened. After `close()` it fails with `Closed`.
        [[nodiscard]] TaskHandle<std::expected<FileEvent, IoError>> next(CancellationToken cancellation = {});
        /// The events as an async stream (ends when the watcher is closed).
        [[nodiscard]] Stream<FileEvent> events();

        [[nodiscard]] bool uses_native_backend() const noexcept;
        void close() noexcept;

      private:
        std::shared_ptr<Detail::WatchCore> core_;
    };

} // namespace SFT::Async
