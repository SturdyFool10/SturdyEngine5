#include <Async/FileWatch.hpp>

#include <Async/Promise.hpp>

#if !defined(STURDY_PLATFORM_WEB)

#include <Async/IoHandler.hpp>
#include <Async/NetInternal.hpp>

#include <deque>
#include <filesystem>
#include <map>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace SFT::Async {

    namespace fs = std::filesystem;

    namespace Detail {

        using Strand = asio::strand<asio::io_context::executor_type>;
        using Clock = std::chrono::steady_clock;
        template <typename T>
        using WResult = std::expected<T, IoError>;

        struct WatchWaiter {
            Promise<WResult<FileEvent>> promise;
            bool done = false;
            CancellationRegistration registration;
        };

        struct PollEntry {
            fs::file_time_type mtime{};
            std::uintmax_t size = 0;
            bool is_directory = false;
        };
        using Snapshot = std::map<std::string, PollEntry>;

        Snapshot take_snapshot(const std::string &root, bool recursive) {
            Snapshot snapshot;
            std::error_code error;
            auto add = [&](const fs::path &path) {
                PollEntry entry;
                entry.is_directory = fs::is_directory(path, error);
                entry.mtime = fs::last_write_time(path, error);
                if (!entry.is_directory) entry.size = fs::file_size(path, error);
                if (!error) snapshot[path.string()] = entry;
                error.clear();
            };
            if (fs::is_directory(root, error)) {
                if (recursive) {
                    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) add(it->path());
                } else {
                    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) add(it->path());
                }
            } else {
                add(root);
            }
            return snapshot;
        }

        struct WatchCore : std::enable_shared_from_this<WatchCore> {
            Strand strand{asio::make_strand(reactor())};
            std::string root;
            FileWatchOptions options;
            bool native = false;
            bool closed = false;

            std::deque<FileEvent> queue;
            std::deque<std::shared_ptr<WatchWaiter>> waiters;
            bool overflow_queued = false;
            std::map<std::string, std::pair<Clock::time_point, bool>> pending_modified; ///< path -> (due, is_directory)
            asio::steady_timer debounce_timer{strand};

            // polling
            Snapshot previous;
            asio::steady_timer poll_timer{strand};

#if defined(__linux__)
            std::optional<asio::posix::stream_descriptor> inotify;
            std::map<int, std::string> watch_paths;
            std::map<std::string, int> path_watches;
            std::vector<char> inotify_buffer = std::vector<char>(16 * 1024);
#endif

            // ---- delivery ----
            void deliver() {
                while (!waiters.empty()) {
                    auto waiter = waiters.front();
                    if (waiter->done) {
                        waiters.pop_front();
                        continue;
                    }
                    if (queue.empty()) break;
                    waiters.pop_front();
                    waiter->done = true;
                    waiter->registration.reset();
                    FileEvent event = std::move(queue.front());
                    queue.pop_front();
                    if (event.kind == FileEventKind::Overflow) overflow_queued = false;
                    (void)waiter->promise.set_value(WResult<FileEvent>(std::move(event)));
                }
            }

            void emit(FileEvent event) {
                if (closed) return;
                if (queue.size() >= options.max_pending_events) {
                    if (!overflow_queued) {
                        overflow_queued = true;
                        FileEvent overflow;
                        overflow.kind = FileEventKind::Overflow;
                        overflow.path = root;
                        queue.push_back(std::move(overflow));
                    }
                } else {
                    queue.push_back(std::move(event));
                }
                deliver();
            }

            /// Entry point for every detected change. Bursts of Modified for one path are held back and merged.
            void push(FileEvent event) {
                if (closed) return;
                if (event.kind == FileEventKind::Modified && options.debounce.count() > 0) {
                    const bool first = pending_modified.empty();
                    auto [it, inserted] = pending_modified.try_emplace(event.path, Clock::now() + options.debounce, event.is_directory);
                    (void)it;
                    if (inserted && first) arm_debounce();
                    return;
                }
                if (event.kind == FileEventKind::Removed) pending_modified.erase(event.path);
                if (event.kind == FileEventKind::Renamed) pending_modified.erase(event.old_path);
                emit(std::move(event));
            }

            void arm_debounce() {
                if (pending_modified.empty() || closed) return;
                auto earliest = Clock::time_point::max();
                for (const auto &[path, due] : pending_modified) earliest = (std::min)(earliest, due.first);
                debounce_timer.expires_at(earliest);
                debounce_timer.async_wait(asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec) {
                    if (ec || self->closed) return;
                    const auto now = Clock::now();
                    for (auto it = self->pending_modified.begin(); it != self->pending_modified.end();) {
                        if (it->second.first <= now) {
                            FileEvent event;
                            event.kind = FileEventKind::Modified;
                            event.path = it->first;
                            event.is_directory = it->second.second;
                            it = self->pending_modified.erase(it);
                            self->emit(std::move(event));
                        } else {
                            ++it;
                        }
                    }
                    self->arm_debounce();
                }));
            }

            // ---- polling backend ----
            void poll_later() {
                if (closed) return;
                poll_timer.expires_after(options.poll_interval);
                poll_timer.async_wait(asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec) {
                    if (ec || self->closed) return;
                    auto scan = IoHandler::submit([root = self->root, recursive = self->options.recursive] { return take_snapshot(root, recursive); });
                    scan.on_done([self, scan]() mutable {
                        asio::post(self->strand, [self, scan]() mutable {
                            if (self->closed) return;
                            try {
                                self->apply_snapshot(scan.wait());
                            } catch (...) {
                            }
                            self->poll_later();
                        });
                    });
                }));
            }

            void apply_snapshot(Snapshot current) {
                for (const auto &[path, entry] : current) {
                    auto old = previous.find(path);
                    if (old == previous.end()) {
                        push({FileEventKind::Created, path, {}, entry.is_directory});
                    } else if (!entry.is_directory && (old->second.mtime != entry.mtime || old->second.size != entry.size)) {
                        push({FileEventKind::Modified, path, {}, false});
                    }
                }
                for (const auto &[path, entry] : previous) {
                    if (!current.contains(path)) push({FileEventKind::Removed, path, {}, entry.is_directory});
                }
                previous = std::move(current);
            }

            // ---- inotify backend ----
#if defined(__linux__)
            static constexpr std::uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO |
                                                   IN_DELETE_SELF | IN_MOVE_SELF | IN_DONT_FOLLOW;

            bool add_watch(const std::string &path) {
                const int wd = inotify_add_watch(inotify->native_handle(), path.c_str(), kMask);
                if (wd < 0) return false;
                watch_paths[wd] = path;
                path_watches[path] = wd;
                return true;
            }

            void add_tree(const std::string &path) {
                std::error_code error;
                if (!fs::is_directory(path, error)) return;
                add_watch(path);
                if (!options.recursive) return;
                for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
                    if (it->is_directory(error)) add_watch(it->path().string());
                }
            }

            void drop_watches_under(const std::string &prefix) {
                for (auto it = path_watches.begin(); it != path_watches.end();) {
                    if (it->first == prefix || (it->first.size() > prefix.size() && it->first.compare(0, prefix.size(), prefix) == 0 && it->first[prefix.size()] == '/')) {
                        inotify_rm_watch(inotify->native_handle(), it->second);
                        watch_paths.erase(it->second);
                        it = path_watches.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            void retarget_watches(const std::string &from, const std::string &to) {
                std::vector<std::pair<std::string, int>> moved;
                for (const auto &[path, wd] : path_watches) {
                    if (path == from || (path.size() > from.size() && path.compare(0, from.size(), from) == 0 && path[from.size()] == '/')) moved.emplace_back(path, wd);
                }
                for (const auto &[path, wd] : moved) {
                    const std::string renamed = to + path.substr(from.size());
                    path_watches.erase(path);
                    path_watches[renamed] = wd;
                    watch_paths[wd] = renamed;
                }
            }

            void read_inotify() {
                if (closed || !inotify) return;
                inotify->async_read_some(asio::buffer(inotify_buffer), asio::bind_executor(strand, [self = shared_from_this()](const ErrorCode &ec, std::size_t n) {
                    if (self->closed) return;
                    if (ec) {
                        if (ec != asio::error::operation_aborted) self->push({FileEventKind::Overflow, self->root, {}, false});
                        return;
                    }
                    self->parse_inotify(n);
                    self->read_inotify();
                }));
            }

            void parse_inotify(std::size_t bytes) {
                struct PendingMove {
                    std::string path;
                    bool is_directory;
                };
                std::map<std::uint32_t, PendingMove> moves;
                for (std::size_t offset = 0; offset + sizeof(inotify_event) <= bytes;) {
                    const auto *raw = reinterpret_cast<const inotify_event *>(inotify_buffer.data() + offset);
                    offset += sizeof(inotify_event) + raw->len;
                    if (raw->mask & IN_Q_OVERFLOW) {
                        push({FileEventKind::Overflow, root, {}, false});
                        continue;
                    }
                    auto base = watch_paths.find(raw->wd);
                    if (base == watch_paths.end()) continue;
                    std::string path = base->second;
                    if (raw->len > 0) path += "/" + std::string(raw->name);
                    const bool is_dir = (raw->mask & IN_ISDIR) != 0;

                    if (raw->mask & IN_IGNORED) {
                        path_watches.erase(base->second);
                        watch_paths.erase(base);
                    } else if (raw->mask & IN_CREATE) {
                        push({FileEventKind::Created, path, {}, is_dir});
                        if (is_dir && options.recursive) {
                            add_tree(path);
                            // Anything created before the new watch existed would otherwise be missed.
                            std::error_code error;
                            for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
                                push({FileEventKind::Created, it->path().string(), {}, it->is_directory(error)});
                            }
                        }
                    } else if (raw->mask & IN_DELETE) {
                        push({FileEventKind::Removed, path, {}, is_dir});
                        if (is_dir) drop_watches_under(path);
                    } else if (raw->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB)) {
                        push({FileEventKind::Modified, path, {}, is_dir});
                    } else if (raw->mask & IN_MOVED_FROM) {
                        moves[raw->cookie] = {path, is_dir};
                    } else if (raw->mask & IN_MOVED_TO) {
                        if (auto from = moves.find(raw->cookie); from != moves.end()) {
                            push({FileEventKind::Renamed, path, from->second.path, is_dir});
                            if (is_dir) retarget_watches(from->second.path, path);
                            moves.erase(from);
                        } else {
                            push({FileEventKind::Created, path, {}, is_dir});
                            if (is_dir && options.recursive) add_tree(path);
                        }
                    } else if (raw->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        if (path == root) push({FileEventKind::Removed, path, {}, is_dir});
                    }
                }
                // A move whose destination never showed up left the watched tree: to us it is a removal.
                for (const auto &[cookie, move] : moves) {
                    push({FileEventKind::Removed, move.path, {}, move.is_directory});
                    if (move.is_directory) drop_watches_under(move.path);
                }
            }
#endif

            void close_now() {
                if (closed) return;
                closed = true;
                ErrorCode ignored;
                debounce_timer.cancel();
                poll_timer.cancel();
#if defined(__linux__)
                if (inotify) inotify->close(ignored);
#endif
                for (auto &waiter : waiters) {
                    if (!waiter->done) {
                        waiter->done = true;
                        waiter->registration.reset();
                        (void)waiter->promise.set_value(WResult<FileEvent>(std::unexpected(IoError{IoErrorCode::Closed, "file watcher closed"})));
                    }
                }
                waiters.clear();
                queue.clear();
                pending_modified.clear();
            }
        };

    } // namespace Detail

    using namespace Detail;

    FileWatcher::~FileWatcher() noexcept { close(); }
    FileWatcher &FileWatcher::operator=(FileWatcher &&other) noexcept {
        if (this != &other) {
            close();
            core_ = std::move(other.core_);
        }
        return *this;
    }

    std::expected<FileWatcher, IoError> FileWatcher::watch(const std::string &path, FileWatchOptions options) {
        std::error_code error;
        const fs::path absolute = fs::absolute(path, error);
        if (error || !fs::exists(absolute, error)) return std::unexpected(IoError{IoErrorCode::NotFound, "cannot watch '" + path + "': no such file or directory"});
        auto core = std::make_shared<WatchCore>();
        core->root = absolute.lexically_normal().string();
        while (core->root.size() > 1 && core->root.back() == '/') core->root.pop_back();
        core->options = options;
        core->options.max_pending_events = (std::max)(core->options.max_pending_events, std::size_t{1});

#if defined(__linux__)
        if (!options.force_polling) {
            const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd < 0) return std::unexpected(IoError{IoErrorCode::Unknown, "inotify_init1 failed"});
            core->inotify.emplace(reactor(), fd);
            core->native = true;
            core->add_tree(core->root);
            if (!fs::is_directory(core->root, error) && !core->add_watch(core->root)) {
                return std::unexpected(IoError{IoErrorCode::PermissionDenied, "cannot watch '" + path + "' (permission or inotify limit)"});
            }
            if (core->path_watches.empty()) return std::unexpected(IoError{IoErrorCode::PermissionDenied, "cannot watch '" + path + "' (permission or inotify limit)"});
            asio::post(core->strand, [core] { core->read_inotify(); });
        }
#endif
        if (!core->native) {
            core->previous = take_snapshot(core->root, options.recursive);
            asio::post(core->strand, [core] { core->poll_later(); });
        }
        FileWatcher watcher;
        watcher.core_ = std::move(core);
        return watcher;
    }

    TaskHandle<std::expected<FileEvent, IoError>> FileWatcher::next(CancellationToken cancellation) {
        Promise<WResult<FileEvent>> promise;
        auto handle = promise.handle();
        if (!core_) {
            (void)promise.set_value(WResult<FileEvent>(std::unexpected(IoError{IoErrorCode::Closed, "no watcher"})));
            return handle;
        }
        if (cancellation.is_cancelled()) {
            (void)promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
            return handle;
        }
        auto waiter = std::make_shared<WatchWaiter>();
        waiter->promise = promise;
        asio::post(core_->strand, [core = core_, waiter, cancellation] {
            if (core->closed) {
                waiter->done = true;
                (void)waiter->promise.set_value(WResult<FileEvent>(std::unexpected(IoError{IoErrorCode::Closed, "file watcher closed"})));
                return;
            }
            if (cancellation.can_be_cancelled()) {
                waiter->registration = cancellation.on_cancel([weak_core = std::weak_ptr<WatchCore>(core), weak_waiter = std::weak_ptr<WatchWaiter>(waiter)] {
                    auto c = weak_core.lock();
                    if (!c) return;
                    asio::post(c->strand, [c, weak_waiter] {
                        if (auto w = weak_waiter.lock(); w && !w->done) {
                            w->done = true;
                            (void)w->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                        }
                    });
                });
            }
            core->waiters.push_back(waiter);
            core->deliver();
        });
        return handle;
    }

    Stream<FileEvent> FileWatcher::events() {
        auto core = core_;
        auto shared = std::make_shared<FileWatcher>();
        shared->core_ = core;
        return stream_from([shared]() -> TaskHandle<std::optional<FileEvent>> {
            auto pending = shared->next();
            Promise<std::optional<FileEvent>> promise;
            pending.on_done([pending, promise]() mutable {
                try {
                    auto result = pending.wait();
                    if (result) (void)promise.set_value(std::optional<FileEvent>(std::move(*result)));
                    else (void)promise.set_value(std::optional<FileEvent>{}); // closed: the stream ends
                } catch (...) {
                    (void)promise.set_exception(std::current_exception());
                }
            });
            return promise.handle();
        });
    }

    bool FileWatcher::uses_native_backend() const noexcept { return core_ && core_->native; }

    void FileWatcher::close() noexcept {
        if (core_) {
            try {
                asio::post(core_->strand, [core = core_] { core->close_now(); });
            } catch (...) {
            }
        }
    }

} // namespace SFT::Async

#else

namespace SFT::Async {
    FileWatcher::~FileWatcher() noexcept {}
    FileWatcher &FileWatcher::operator=(FileWatcher &&other) noexcept { core_ = std::move(other.core_); return *this; }
    std::expected<FileWatcher, IoError> FileWatcher::watch(const std::string &, FileWatchOptions) { return std::unexpected(IoError{IoErrorCode::Unsupported, "file watching is not available on Web"}); }
    TaskHandle<std::expected<FileEvent, IoError>> FileWatcher::next(CancellationToken) {
        Promise<std::expected<FileEvent, IoError>> p;
        (void)p.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "file watching is not available on Web"}));
        return p.handle();
    }
    Stream<FileEvent> FileWatcher::events() { return stream_empty<FileEvent>(); }
    bool FileWatcher::uses_native_backend() const noexcept { return false; }
    void FileWatcher::close() noexcept {}
} // namespace SFT::Async

#endif
