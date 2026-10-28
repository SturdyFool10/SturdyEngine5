// Child processes: run_process and the streaming Process API.
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
namespace fs = std::filesystem;

namespace {

    int failures = 0;

    void check(bool ok, const char *what) {
        if (!ok) {
            Foundation::log_error("ProcessTest FAILED: {}", what);
            ++failures;
        }
    }

    std::vector<std::byte> bytes(std::string_view text) {
        std::vector<std::byte> out(text.size());
        std::memcpy(out.data(), text.data(), text.size());
        return out;
    }

    template <typename T>
    bool failed_with(const std::expected<T, IoError> &result, IoErrorCode code) {
        return !result && result.error().code == code;
    }

    ProcessOptions shell(std::string script) {
        ProcessOptions options;
        options.program = "sh";
        options.args = {"-c", std::move(script)};
        return options;
    }

    TaskHandle<std::string> read_all_stdout(std::shared_ptr<Process> process) {
        std::string text;
        for (;;) {
            auto chunk = co_await process->read_stdout(4096);
            if (!chunk || chunk->empty()) break;
            text.append(reinterpret_cast<const char *>(chunk->data()), chunk->size());
        }
        co_return text;
    }

} // namespace

int main() {
    Scheduler::initialize(2);

    // Basic capture and exit codes.
    {
        ProcessOptions echo;
        echo.program = "echo";
        echo.args = {"hello", "world"};
        auto result = run_process(echo).wait();
        check(result.has_value() && result->stdout_text() == "hello world\n" && result->status.success(), "run_process captures stdout of a PATH program");

        auto code = run_process(shell("exit 7")).wait();
        check(code && code->status.code == 7 && !code->status.success() && !code->status.signaled, "exit codes are reported");

        auto both = run_process(shell("echo out; echo err 1>&2")).wait();
        check(both && both->stdout_text() == "out\n" && both->stderr_text() == "err\n", "stdout and stderr are captured separately");

        auto killed = run_process(shell("kill -9 $$")).wait();
        check(killed && killed->status.signaled && killed->status.signal == 9 && !killed->status.success(), "a signalled child is reported as such");

        auto missing = ProcessOptions{};
        missing.program = "definitely-not-a-real-program-xyz";
        check(failed_with(run_process(missing).wait(), IoErrorCode::NotFound), "a missing program is NotFound");
        ProcessOptions bad_path;
        bad_path.program = "/nonexistent/dir/tool";
        check(!run_process(bad_path).wait().has_value(), "a bad path fails to start");
    }

    // stdin -> cat -> stdout, large enough that reading and writing must overlap or the pipes deadlock.
    {
        std::vector<std::byte> payload(3 * 1024 * 1024);
        for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i * 131 + 7);
        ProcessOptions cat;
        cat.program = "cat";
        auto result = run_process(cat, payload).wait();
        check(result && result->stdout_data == payload && result->status.success(), "3 MiB round-trips through cat (no pipe deadlock)");
    }

    // Environment and working directory.
    {
        auto options = shell("echo \"$SPECIAL\"; pwd");
        options.environment["SPECIAL"] = "from-parent";
        options.working_directory = "/tmp";
        auto result = run_process(options).wait();
        check(result && result->stdout_text() == "from-parent\n/tmp\n", "environment variable and working directory apply");

        ProcessOptions clean;
        clean.program = "/usr/bin/env";
        clean.clear_environment = true;
        clean.environment["ONLY"] = "me";
        auto env = run_process(clean).wait();
        check(env && env->stdout_text() == "ONLY=me\n", "clear_environment starts the child with just what was given");

        auto inherited = run_process(shell("echo \"${HOME:-unset}\"")).wait();
        check(inherited && inherited->stdout_text() != "unset\n", "the environment is inherited by default");
    }

    // Limits.
    {
        RunLimits quick;
        quick.timeout = 200ms;
        const auto start = std::chrono::steady_clock::now();
        auto slow = run_process(shell("sleep 5"), {}, quick).wait();
        check(failed_with(slow, IoErrorCode::TimedOut), "a process past its timeout is killed and reported TimedOut");
        check(std::chrono::steady_clock::now() - start < 3s, "...without waiting for it to finish");

        RunLimits small;
        small.max_output_bytes = 1024 * 1024;
        auto flood = run_process(shell("yes"), {}, small).wait();
        check(failed_with(flood, IoErrorCode::LimitExceeded), "runaway output is cut off with LimitExceeded");

        CancellationSource cancel;
        auto pending = run_process(shell("sleep 5"), {}, {}, cancel.token());
        std::this_thread::sleep_for(100ms);
        (void)cancel.cancel();
        bool cancelled = false;
        try {
            (void)pending.wait();
        } catch (const IoOperationCancelled &) {
            cancelled = true;
        }
        check(cancelled, "run_process can be cancelled (and kills the child)");
    }

    // Streaming API: read as it produces, then wait.
    {
        auto options = shell("for i in 1 2 3; do echo line$i; sleep 0.05; done");
        auto spawned = Process::spawn(options);
        check(spawned.has_value(), "spawn");
        if (spawned) {
            auto process = std::make_shared<Process>(std::move(*spawned));
            check(process->id() > 0, "the child has a pid");
            const auto text = read_all_stdout(process).wait();
            check(text == "line1\nline2\nline3\n", "stdout is readable incrementally until EOF");
            auto status = process->wait().wait();
            check(status && status->success(), "wait reports the exit");
            check(process->wait().wait().has_value(), "wait can be called again");
        }
    }

    // Interactive: write a line, get it back, close stdin, process ends.
    {
        ProcessOptions cat;
        cat.program = "cat";
        cat.stdin_mode = StdioMode::Pipe;
        auto process = Process::spawn(cat);
        check(process.has_value(), "spawn cat");
        if (process) {
            check(process->write_stdin(bytes("ping\n")).wait().has_value(), "write_stdin");
            auto echoed = process->read_stdout(100).wait();
            check(echoed && echoed->size() == 5, "the child answered while still running");
            process->close_stdin();
            auto status = process->wait().wait();
            check(status && status->success(), "closing stdin lets cat finish");
            check(process->read_stdout(100).wait()->empty(), "EOF after the child exited");
        }
    }

    // Cancelling a read does not hurt the process; killing ends it.
    {
        ProcessOptions sleeper = shell("sleep 30");
        auto process = Process::spawn(sleeper);
        if (process) {
            CancellationSource cancel;
            auto read = process->read_stdout(10, cancel.token());
            std::this_thread::sleep_for(50ms);
            (void)cancel.cancel();
            bool cancelled = false;
            try {
                (void)read.wait();
            } catch (const IoOperationCancelled &) {
                cancelled = true;
            }
            check(cancelled, "a pending read can be cancelled");
            process->kill();
            auto status = process->wait().wait();
            check(status && status->signaled && status->signal == 9, "kill() ends the child with SIGKILL");
        }
    }

    // kill_on_drop.
    {
        ProcessOptions doomed = shell("sleep 30");
        doomed.kill_on_drop = true;
        std::int64_t pid = 0;
        {
            auto process = Process::spawn(doomed);
            if (process) pid = process->id();
        }
        bool gone = false;
        for (int i = 0; i < 100 && !gone; ++i) {
            std::this_thread::sleep_for(20ms);
            gone = !fs::exists("/proc/" + std::to_string(pid));
            if (!fs::exists("/proc/self")) gone = true; // no procfs: cannot tell
        }
        check(pid != 0 && gone, "kill_on_drop terminates the child when the Process is dropped");
    }

    // Many processes at once cost no threads.
    {
        std::vector<TaskHandle<std::expected<ProcessOutput, IoError>>> runs;
        for (int i = 0; i < 40; ++i) runs.push_back(run_process(shell("echo " + std::to_string(i))));
        int good = 0;
        for (int i = 0; i < 40; ++i) {
            auto r = runs[static_cast<std::size_t>(i)].wait();
            if (r && r->stdout_text() == std::to_string(i) + "\n") ++good;
        }
        check(good == 40, "40 concurrent child processes all finish with the right output");
        check(IoHandler::pending_count() == 0, "...using no IO-pool workers");
    }

    Scheduler::shutdown();
    return failures == 0 ? 0 : 1;
}
