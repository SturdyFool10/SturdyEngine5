/// `sturdy_log_message`: foreign code writes into the engine's own log. A message reaches the registered sinks at the severity the
/// engine would give it (informational levels all at the lowest, trace), honours the application's log level, and bad arguments are
/// rejected with a reason.

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <Foundation/Foundation.hpp>
#include <FFI/Sturdy.h>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "LogMessageFfiTest: %s\n", description);
            ++failures;
        }
    }

    struct Captured {
        std::mutex mutex;
        std::vector<std::pair<SturdyLogLevel, std::string>> messages;
    };

    void STURDY_ABI_CALL capture(SturdyLogLevel level, const char *message, size_t length, void *user_data) {
        auto &captured = *static_cast<Captured *>(user_data);
        const std::lock_guard<std::mutex> lock{captured.mutex};
        captured.messages.emplace_back(level, std::string{message, length});
    }

    bool saw(Captured &captured, SturdyLogLevel level, const char *text) {
        const std::lock_guard<std::mutex> lock{captured.mutex};
        for (const auto &[seen_level, seen_text] : captured.messages) {
            if (seen_level == level && seen_text.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

} // namespace

int main() {
    Captured captured;
    SturdyLogSink sink{};
    check(sturdy_log_add_sink(capture, &captured, &sink) == STURDY_OK, "a sink registers");

    SFT::Foundation::set_log_level(SFT::Foundation::LogLevel::Trace);

    const auto send = [](SturdyLogLevel level, const char *text) { return sturdy_log_message(level, text, std::char_traits<char>::length(text)); };
    check(send(STURDY_LOG_LEVEL_TRACE, "trace marker") == STURDY_OK, "trace is accepted");
    check(send(STURDY_LOG_LEVEL_DEBUG, "debug marker") == STURDY_OK, "debug is accepted");
    check(send(STURDY_LOG_LEVEL_INFO, "info marker") == STURDY_OK, "info is accepted");
    check(send(STURDY_LOG_LEVEL_WARN, "warn marker") == STURDY_OK, "warn is accepted");
    check(send(STURDY_LOG_LEVEL_ERROR, "error marker") == STURDY_OK, "error is accepted");
    check(send(STURDY_LOG_LEVEL_CRITICAL, "critical marker") == STURDY_OK, "critical is accepted");

    check(saw(captured, STURDY_LOG_LEVEL_TRACE, "trace marker"), "trace arrives at trace");
    check(saw(captured, STURDY_LOG_LEVEL_TRACE, "debug marker"), "debug is emitted at the lowest severity");
    check(saw(captured, STURDY_LOG_LEVEL_TRACE, "info marker"), "info is emitted at the lowest severity");
    check(saw(captured, STURDY_LOG_LEVEL_WARN, "warn marker"), "warn keeps its severity");
    check(saw(captured, STURDY_LOG_LEVEL_ERROR, "error marker"), "error keeps its severity");
    check(saw(captured, STURDY_LOG_LEVEL_ERROR, "critical marker"), "critical is reported as an error");

    // The message is not required to be null-terminated: only `length` bytes are read.
    const char padded[] = {'a', 'b', 'c', 'X', 'Y'};
    check(sturdy_log_message(STURDY_LOG_LEVEL_WARN, padded, 3) == STURDY_OK, "an unterminated buffer is accepted");
    check(saw(captured, STURDY_LOG_LEVEL_WARN, "abc") && !saw(captured, STURDY_LOG_LEVEL_WARN, "abcX"), "only message_length bytes are read");

    check(sturdy_log_message(STURDY_LOG_LEVEL_WARN, nullptr, 0) == STURDY_OK, "an empty message with a null pointer is accepted");
    check(sturdy_log_message(STURDY_LOG_LEVEL_WARN, nullptr, 4) == STURDY_ERROR_INVALID_ARGUMENT, "a null pointer with a length is rejected");
    check(std::string{sturdy_last_error_message()}.find("message") != std::string::npos, "the rejection says why");
    check(sturdy_log_message(STURDY_LOG_LEVEL_FORCE_U32, "x", 1) == STURDY_ERROR_INVALID_ARGUMENT, "an unknown level is rejected");

    // At the default level the lowest severity is filtered out, as for the engine's own informational messages.
    SFT::Foundation::set_log_level(SFT::Foundation::LogLevel::Info);
    check(send(STURDY_LOG_LEVEL_INFO, "hidden marker") == STURDY_OK, "info is accepted at the default level");
    check(!saw(captured, STURDY_LOG_LEVEL_TRACE, "hidden marker"), "the default level filters trace");

    check(sturdy_log_remove_sink(sink) == STURDY_OK, "the sink unregisters");

    if (failures != 0) {
        (void)std::fprintf(stderr, "LogMessageFfiTest: %d failure(s)\n", failures);
        return 1;
    }
    (void)std::printf("LogMessageFfiTest: ok\n");
    return 0;
}
