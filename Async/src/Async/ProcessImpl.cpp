#include <Async/Process.hpp>

#include <Async/Promise.hpp>
#include <Async/Timer.hpp>

#include <atomic>
#include <deque>

#if !defined(STURDY_PLATFORM_WEB) && defined(STURDY_ASYNC_PROCESS)

#include <Async/NetInternal.hpp>

#include <boost/process.hpp>
#include <boost/process/v2/posix/vfork_launcher.hpp>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace SFT::Async {

    namespace bp = boost::process;

    namespace Detail {

        using Bytes = std::vector<std::byte>;
        using Strand = asio::strand<asio::io_context::executor_type>;

        [[nodiscard]] inline ExitStatus to_exit_status(int native) {
            ExitStatus status;
#if defined(_WIN32)
            status.code = native;
#else
            if (WIFSIGNALED(native)) {
                status.signaled = true;
                status.signal = WTERMSIG(native);
                status.code = status.signal;
            } else if (WIFEXITED(native)) {
                status.code = WEXITSTATUS(native);
            } else {
                status.code = native;
            }
#endif
            return status;
        }

        struct ReadRequest {
            std::size_t max_bytes;
            Promise<Result<Bytes>> promise;
            bool done = false;
            CancellationRegistration registration;
        };

        /// One readable pipe with an ordered queue of read requests.
        struct PipeReader {
            asio::readable_pipe pipe{reactor()};
            bool open = false;
            bool reading = false;
            bool eof = false;
            Bytes buffer = Bytes(64 * 1024);
            std::deque<std::shared_ptr<ReadRequest>> queue;
        };

        struct WriteRequest {
            Bytes data;
            Promise<Result<void>> promise;
            bool done = false;
            CancellationRegistration registration;
        };

        struct ProcessCore : std::enable_shared_from_this<ProcessCore> {
            Strand strand{asio::make_strand(reactor())};
            std::optional<bp::process> process;
            asio::writable_pipe stdin_pipe{reactor()};
            bool stdin_open = false;
            bool writing = false;
            std::deque<std::shared_ptr<WriteRequest>> writes;
            PipeReader out;
            PipeReader err;
            Promise<Result<ExitStatus>> exit;
            std::atomic<bool> exited{false};
            std::int64_t pid = 0;
            bool kill_on_drop = false;

            ~ProcessCore() {
                if (process) {
                    boost::system::error_code ignored;
                    if (kill_on_drop && !exited.load()) process->terminate(ignored);
                    else if (!exited.load()) process->detach();
                }
            }

            // ---- reading ----
            void pump_reads(PipeReader &reader) {
                while (!reader.queue.empty() && reader.queue.front()->done) reader.queue.pop_front();
                if (reader.queue.empty() || reader.reading || !reader.open) return;
                auto request = reader.queue.front();
                if (reader.eof) {
                    reader.queue.pop_front();
                    request->done = true;
                    request->registration.reset();
                    (void)request->promise.set_value(Result<Bytes>(Bytes{}));
                    return pump_reads(reader);
                }
                reader.reading = true;
                const std::size_t want = (std::min)(request->max_bytes, reader.buffer.size());
                reader.pipe.async_read_some(asio::buffer(reader.buffer.data(), want),
                                            asio::bind_executor(strand, [self = shared_from_this(), &reader, request](const ErrorCode &ec, std::size_t n) {
                                                reader.reading = false;
                                                if (request->done) { // cancelled while in flight: nothing was consumed
                                                    self->pump_reads(reader);
                                                    return;
                                                }
                                                if (ec == asio::error::eof || ec == asio::error::broken_pipe) {
                                                    reader.eof = true;
                                                } else if (ec == asio::error::operation_aborted) {
                                                    self->pump_reads(reader);
                                                    return;
                                                } else if (ec) {
                                                    request->done = true;
                                                    request->registration.reset();
                                                    std::erase(reader.queue, request);
                                                    (void)request->promise.set_value(Result<Bytes>(std::unexpected(to_io_error("read pipe", ec))));
                                                    self->pump_reads(reader);
                                                    return;
                                                }
                                                if (n > 0) {
                                                    request->done = true;
                                                    request->registration.reset();
                                                    std::erase(reader.queue, request);
                                                    (void)request->promise.set_value(Result<Bytes>(Bytes(reader.buffer.begin(), reader.buffer.begin() + static_cast<std::ptrdiff_t>(n))));
                                                }
                                                self->pump_reads(reader);
                                            }));
            }

            TaskHandle<Result<Bytes>> read(PipeReader &reader, std::size_t max_bytes, CancellationToken cancellation) {
                auto request = std::make_shared<ReadRequest>();
                request->max_bytes = (std::max)(max_bytes, std::size_t{1});
                auto handle = request->promise.handle();
                if (!reader.open) {
                    (void)request->promise.set_value(Result<Bytes>(std::unexpected(IoError{IoErrorCode::InvalidArgument, "that stream is not piped"})));
                    return handle;
                }
                if (cancellation.is_cancelled()) {
                    (void)request->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                    return handle;
                }
                asio::post(strand, [self = shared_from_this(), &reader, request, cancellation] {
                    if (cancellation.can_be_cancelled()) {
                        request->registration = cancellation.on_cancel([weak = std::weak_ptr<ProcessCore>(self), &reader, weak_request = std::weak_ptr<ReadRequest>(request)] {
                            auto core = weak.lock();
                            if (!core) return;
                            asio::post(core->strand, [core, &reader, weak_request] {
                                auto r = weak_request.lock();
                                if (!r || r->done) return;
                                r->done = true;
                                (void)r->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                                if (reader.reading && !reader.queue.empty() && reader.queue.front() == r) {
                                    boost::system::error_code ignored;
                                    reader.pipe.cancel(ignored); // an in-flight read has consumed nothing yet
                                }
                            });
                        });
                    }
                    reader.queue.push_back(request);
                    self->pump_reads(reader);
                });
                return handle;
            }

            // ---- writing ----
            void pump_writes() {
                while (!writes.empty() && writes.front()->done) writes.pop_front();
                if (writes.empty() || writing) return;
                if (!stdin_open) {
                    auto request = writes.front();
                    writes.pop_front();
                    request->done = true;
                    (void)request->promise.set_value(Result<void>(std::unexpected(IoError{IoErrorCode::Closed, "stdin is closed or not piped"})));
                    return pump_writes();
                }
                auto request = writes.front();
                writing = true;
                asio::async_write(stdin_pipe, asio::buffer(request->data),
                                  asio::bind_executor(strand, [self = shared_from_this(), request](const ErrorCode &ec, std::size_t) {
                                      self->writing = false;
                                      if (!self->writes.empty() && self->writes.front() == request) self->writes.pop_front();
                                      if (!request->done) {
                                          request->done = true;
                                          request->registration.reset();
                                          if (ec) (void)request->promise.set_value(Result<void>(std::unexpected(to_io_error("write stdin", ec))));
                                          else (void)request->promise.set_value(Result<void>());
                                      }
                                      self->pump_writes();
                                  }));
            }

            void close_stdin_now() {
                if (!stdin_open) return;
                stdin_open = false;
                boost::system::error_code ignored;
                stdin_pipe.close(ignored);
                auto pending = std::move(writes);
                writes.clear();
                for (auto &w : pending)
                    if (!w->done) {
                        w->done = true;
                        (void)w->promise.set_value(Result<void>(std::unexpected(IoError{IoErrorCode::Closed, "stdin was closed"})));
                    }
            }
        };

        template <typename Binding, typename Pipe>
        Binding make_binding(StdioMode mode, Pipe &pipe) {
            switch (mode) {
                case StdioMode::Pipe: return Binding(pipe);
                case StdioMode::Null: return Binding(nullptr);
                default: return Binding();
            }
        }

    } // namespace Detail

    using namespace Detail;

    namespace {
        /// The pending exit watch keeps the core alive for as long as the child runs, so "drop" behaviour must be acted on
        /// by the owning `Process` object, not by the core's destructor.
        void drop_process(const std::shared_ptr<ProcessCore> &core) noexcept {
            if (!core || !core->kill_on_drop) return;
            try {
                asio::post(core->strand, [core] {
                    boost::system::error_code ignored;
                    if (core->process && !core->exited.load()) core->process->terminate(ignored);
                });
            } catch (...) {
            }
        }
    } // namespace

    Process::~Process() noexcept { drop_process(core_); }
    Process &Process::operator=(Process &&other) noexcept {
        if (this != &other) {
            drop_process(core_);
            core_ = std::move(other.core_);
        }
        return *this;
    }

    std::expected<Process, IoError> Process::spawn(const ProcessOptions &options) {
        if (options.program.empty()) return std::unexpected(IoError{IoErrorCode::InvalidArgument, "ProcessOptions::program is empty"});
        auto core = std::make_shared<ProcessCore>();
        core->kill_on_drop = options.kill_on_drop;
        core->stdin_open = options.stdin_mode == StdioMode::Pipe;
        core->out.open = options.stdout_mode == StdioMode::Pipe;
        core->err.open = options.stderr_mode == StdioMode::Pipe;
        try {
            bp::filesystem::path program(options.program);
            if (program.filename() == program) { // bare name: search PATH
                auto found = bp::environment::find_executable(options.program);
                if (found.empty()) return std::unexpected(IoError{IoErrorCode::NotFound, "program '" + options.program + "' not found on PATH"});
                program = found;
            }
            auto stdio = bp::process_stdio{make_binding<decltype(bp::process_stdio::in)>(options.stdin_mode, core->stdin_pipe),
                                           make_binding<decltype(bp::process_stdio::out)>(options.stdout_mode, core->out.pipe),
                                           make_binding<decltype(bp::process_stdio::err)>(options.stderr_mode, core->err.pipe)};

            // Environment as plain "KEY=value" strings first (easy to filter), then handed to Boost.Process.
            std::vector<std::string> variables;
            if (!options.clear_environment) {
                for (const auto &entry : bp::environment::current()) variables.emplace_back(entry.native().data(), entry.native().size());
            }
            for (const auto &[key, value] : options.environment) {
                std::erase_if(variables, [&](const std::string &v) { return v.size() > key.size() && v.compare(0, key.size(), key) == 0 && v[key.size()] == '='; });
                variables.push_back(key + "=" + value);
            }
            std::vector<bp::environment::key_value_pair> environment(variables.begin(), variables.end());
            const bool custom_environment = options.clear_environment || !options.environment.empty();
            const bool custom_directory = !options.working_directory.empty();

            // vfork_launcher, not the default one: after fork the default launcher calls the reactor's `notify_fork(fork_child)`,
            // which re-registers every descriptor the reactor knows about -- and one that another reactor thread closed a
            // moment earlier fails that with EBADF ("epoll re-registration"), so a spawn racing with socket churn failed at
            // random. The child only execs, so it needs no reactor fix-up at all.
            boost::process::v2::posix::vfork_launcher launcher;
            if (custom_environment && custom_directory) {
                core->process.emplace(launcher(reactor(), program, options.args, std::move(stdio), bp::process_environment(environment),
                                               bp::process_start_dir(options.working_directory)));
            } else if (custom_environment) {
                core->process.emplace(launcher(reactor(), program, options.args, std::move(stdio), bp::process_environment(environment)));
            } else if (custom_directory) {
                core->process.emplace(launcher(reactor(), program, options.args, std::move(stdio), bp::process_start_dir(options.working_directory)));
            } else {
                core->process.emplace(launcher(reactor(), program, options.args, std::move(stdio)));
            }
        } catch (const boost::system::system_error &error) {
            const int value = error.code().value();
            IoErrorCode code = IoErrorCode::Unknown;
            if (value == ENOENT) code = IoErrorCode::NotFound;
            else if (value == EACCES || value == EPERM) code = IoErrorCode::PermissionDenied;
            return std::unexpected(IoError{code, std::string("cannot start '") + options.program + "': " + error.what()});
        } catch (const std::exception &error) {
            return std::unexpected(IoError{IoErrorCode::Unknown, std::string("cannot start '") + options.program + "': " + error.what()});
        }
        // The child's ends of the pipes were handed over by the launcher; ours stay open in `core`.
        core->pid = static_cast<std::int64_t>(core->process->id());
        core->process->async_wait(asio::bind_executor(core->strand, [core](const ErrorCode &ec, int) {
            core->exited.store(true);
            if (ec) (void)core->exit.set_value(Result<ExitStatus>(std::unexpected(to_io_error("wait for process", ec))));
            else (void)core->exit.set_value(Result<ExitStatus>(to_exit_status(core->process->native_exit_code()))); // the raw wait status, unlike `native`
        }));
        Process out;
        out.core_ = std::move(core);
        return out;
    }

    std::int64_t Process::id() const noexcept { return core_ ? core_->pid : 0; }

    TaskHandle<std::expected<void, IoError>> Process::write_stdin(std::vector<std::byte> data, CancellationToken cancellation) {
        auto request = std::make_shared<WriteRequest>();
        request->data = std::move(data);
        auto handle = request->promise.handle();
        if (!core_) {
            (void)request->promise.set_value(Result<void>(std::unexpected(IoError{IoErrorCode::Closed, "no process"})));
            return handle;
        }
        asio::post(core_->strand, [core = core_, request, cancellation] {
            if (cancellation.can_be_cancelled()) {
                request->registration = cancellation.on_cancel([weak = std::weak_ptr<ProcessCore>(core), weak_request = std::weak_ptr<WriteRequest>(request)] {
                    auto c = weak.lock();
                    if (!c) return;
                    asio::post(c->strand, [c, weak_request] {
                        auto r = weak_request.lock();
                        if (!r || r->done) return;
                        r->done = true;
                        (void)r->promise.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
                        if (c->writing && !c->writes.empty() && c->writes.front() == r) {
                            boost::system::error_code ignored;
                            c->stdin_pipe.cancel(ignored); // the message may be left half written
                        }
                    });
                });
            }
            core->writes.push_back(request);
            core->pump_writes();
        });
        return handle;
    }

    void Process::close_stdin() noexcept {
        if (core_) asio::post(core_->strand, [core = core_] { core->close_stdin_now(); });
    }

    TaskHandle<std::expected<std::vector<std::byte>, IoError>> Process::read_stdout(std::size_t max_bytes, CancellationToken cancellation) {
        if (!core_) {
            Promise<Result<Bytes>> p;
            (void)p.set_value(Result<Bytes>(std::unexpected(IoError{IoErrorCode::Closed, "no process"})));
            return p.handle();
        }
        return core_->read(core_->out, max_bytes, std::move(cancellation));
    }

    TaskHandle<std::expected<std::vector<std::byte>, IoError>> Process::read_stderr(std::size_t max_bytes, CancellationToken cancellation) {
        if (!core_) {
            Promise<Result<Bytes>> p;
            (void)p.set_value(Result<Bytes>(std::unexpected(IoError{IoErrorCode::Closed, "no process"})));
            return p.handle();
        }
        return core_->read(core_->err, max_bytes, std::move(cancellation));
    }

    TaskHandle<std::expected<ExitStatus, IoError>> Process::wait(CancellationToken cancellation) {
        if (!core_) {
            Promise<Result<ExitStatus>> p;
            (void)p.set_value(Result<ExitStatus>(std::unexpected(IoError{IoErrorCode::Closed, "no process"})));
            return p.handle();
        }
        if (!cancellation.can_be_cancelled()) return core_->exit.handle();
        Promise<Result<ExitStatus>> mine;
        auto registration = std::make_shared<CancellationRegistration>(cancellation.on_cancel([mine]() mutable {
            (void)mine.set_exception(std::make_exception_ptr(IoOperationCancelled{}));
        }));
        auto exit = core_->exit.handle();
        exit.on_done([exit, mine, registration]() mutable {
            try {
                (void)mine.set_value(exit.wait());
            } catch (...) {
                (void)mine.set_exception(std::current_exception());
            }
        });
        return mine.handle();
    }

    void Process::request_exit() noexcept {
        if (core_) asio::post(core_->strand, [core = core_] {
            boost::system::error_code ignored;
            if (core->process && !core->exited.load()) core->process->request_exit(ignored);
        });
    }

    void Process::kill() noexcept {
        if (core_) asio::post(core_->strand, [core = core_] {
            boost::system::error_code ignored;
            if (core->process && !core->exited.load()) core->process->terminate(ignored);
        });
    }

    // =============================================================================================
    // run_process
    // =============================================================================================
    namespace {

        struct RunState {
            std::shared_ptr<Process> process;
            std::atomic<int> reason{0}; // 1 timed out, 2 cancelled, 3 too much output
            std::atomic<std::size_t> collected{0};
            std::size_t limit = 0;
        };

        TaskHandle<std::vector<std::byte>> drain(std::shared_ptr<RunState> state, bool is_stdout) {
            std::vector<std::byte> all;
            for (;;) {
                auto chunk = co_await (is_stdout ? state->process->read_stdout(64 * 1024) : state->process->read_stderr(64 * 1024));
                if (!chunk || chunk->empty()) break;
                if (state->collected.fetch_add(chunk->size()) + chunk->size() > state->limit) {
                    int expected = 0;
                    state->reason.compare_exchange_strong(expected, 3);
                    state->process->kill();
                    break;
                }
                all.insert(all.end(), chunk->begin(), chunk->end());
            }
            co_return all;
        }

        TaskHandle<void> feed(std::shared_ptr<RunState> state, std::vector<std::byte> data) {
            if (!data.empty()) (void)co_await state->process->write_stdin(std::move(data));
            state->process->close_stdin();
        }

        TaskHandle<std::expected<ProcessOutput, IoError>> run_impl(ProcessOptions options, std::vector<std::byte> stdin_data, RunLimits limits,
                                                                    CancellationToken cancellation) {
            options.stdout_mode = StdioMode::Pipe;
            options.stderr_mode = StdioMode::Pipe;
            options.stdin_mode = stdin_data.empty() ? StdioMode::Null : StdioMode::Pipe;
            auto spawned = Process::spawn(options);
            if (!spawned) co_return std::unexpected(spawned.error());

            auto state = std::make_shared<RunState>();
            state->process = std::make_shared<Process>(std::move(*spawned));
            state->limit = limits.max_output_bytes;

            CancellationSource timer_stop;
            if (limits.timeout.count() > 0) {
                auto timer = sleep_for(limits.timeout, timer_stop.token());
                timer.on_done([timer, state]() mutable {
                    try {
                        timer.wait();
                        int expected = 0;
                        state->reason.compare_exchange_strong(expected, 1);
                        state->process->kill();
                    } catch (...) {
                    }
                });
            }
            auto registration = std::make_shared<CancellationRegistration>(cancellation.on_cancel([state] {
                int expected = 0;
                state->reason.compare_exchange_strong(expected, 2);
                state->process->kill();
            }));

            auto writer = feed(state, std::move(stdin_data));
            auto out = drain(state, true);
            auto err = drain(state, false);
            ProcessOutput output;
            output.stdout_data = co_await out;
            output.stderr_data = co_await err;
            co_await writer;
            auto status = co_await state->process->wait();
            (void)timer_stop.cancel();
            registration->reset();
            switch (state->reason.load()) {
                case 1: co_return std::unexpected(IoError{IoErrorCode::TimedOut, "process exceeded its time limit and was killed"});
                case 2: throw IoOperationCancelled{};
                case 3: co_return std::unexpected(IoError{IoErrorCode::LimitExceeded, "process output exceeded the limit and it was killed"});
                default: break;
            }
            if (!status) co_return std::unexpected(status.error());
            output.status = *status;
            co_return std::expected<ProcessOutput, IoError>(std::move(output));
        }

    } // namespace

    TaskHandle<std::expected<ProcessOutput, IoError>> run_process(ProcessOptions options, std::vector<std::byte> stdin_data, RunLimits limits,
                                                                  CancellationToken cancellation) {
        return run_impl(std::move(options), std::move(stdin_data), limits, std::move(cancellation));
    }

} // namespace SFT::Async

#else // no Boost.Process (or Web)

namespace SFT::Async {
    namespace {
        template <typename T>
        TaskHandle<std::expected<T, IoError>> no_process() {
            Promise<std::expected<T, IoError>> p;
            (void)p.set_value(std::unexpected(IoError{IoErrorCode::Unsupported, "child processes are not available in this build"}));
            return p.handle();
        }
    } // namespace
    Process::~Process() noexcept = default;
    Process &Process::operator=(Process &&other) noexcept { core_ = std::move(other.core_); return *this; }
    std::expected<Process, IoError> Process::spawn(const ProcessOptions &) { return std::unexpected(IoError{IoErrorCode::Unsupported, "child processes are not available in this build"}); }
    std::int64_t Process::id() const noexcept { return 0; }
    TaskHandle<std::expected<void, IoError>> Process::write_stdin(std::vector<std::byte>, CancellationToken) { return no_process<void>(); }
    void Process::close_stdin() noexcept {}
    TaskHandle<std::expected<std::vector<std::byte>, IoError>> Process::read_stdout(std::size_t, CancellationToken) { return no_process<std::vector<std::byte>>(); }
    TaskHandle<std::expected<std::vector<std::byte>, IoError>> Process::read_stderr(std::size_t, CancellationToken) { return no_process<std::vector<std::byte>>(); }
    TaskHandle<std::expected<ExitStatus, IoError>> Process::wait(CancellationToken) { return no_process<ExitStatus>(); }
    void Process::request_exit() noexcept {}
    void Process::kill() noexcept {}
    TaskHandle<std::expected<ProcessOutput, IoError>> run_process(ProcessOptions, std::vector<std::byte>, RunLimits, CancellationToken) { return no_process<ProcessOutput>(); }
} // namespace SFT::Async

#endif
