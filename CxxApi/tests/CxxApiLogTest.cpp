// The CxxApi log sink: a registered callback receives engine log messages with their level, and stops receiving them once
// removed; the callback is destroyed on removal.

#include <CxxApi/CxxApi.hpp>
#include <CxxApi/Error.hpp>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <Foundation/Foundation.hpp>

using namespace SFT;
using namespace SFT::CxxApi;

namespace {

    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }

    struct Captured {
        std::mutex mutex;
        std::vector<std::pair<LogLevel, std::string>> messages;
        int destroyed = 0;
    };

    class Capture final : public LogSinkCallback {
      public:
        explicit Capture(Captured &out) : out_(out) {}
        ~Capture() override {
            const std::lock_guard<std::mutex> lock{out_.mutex};
            ++out_.destroyed;
        }
        void on_message(LogLevel level, const char *message, std::size_t length) noexcept override {
            const std::lock_guard<std::mutex> lock{out_.mutex};
            out_.messages.emplace_back(level, std::string{message, length});
        }

      private:
        Captured &out_;
    };

    bool saw(Captured &c, LogLevel level, const char *needle) {
        const std::lock_guard<std::mutex> lock{c.mutex};
        for (const auto &[l, text] : c.messages) {
            if (l == level && text.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

} // namespace

int main() {
    Captured captured;
    const std::uint64_t sink = log_add_sink(std::make_unique<Capture>(captured));
    check(sink != 0, "a sink registers and returns a handle");

    bool threw = false;
    try {
        (void)log_add_sink(nullptr);
    } catch (const Error &) {
        threw = true;
    }
    check(threw, "a null callback throws");

    Foundation::log_warn("cxxapi-log-test warning marker");
    Foundation::log_info("cxxapi-log-test info marker");
    check(saw(captured, LogLevel::Warn, "warning marker"), "a warning arrives with the Warn level");
    check(saw(captured, LogLevel::Info, "info marker"), "an info message arrives with the Info level");

    log_remove_sink(sink);
    {
        const std::lock_guard<std::mutex> lock{captured.mutex};
        check(captured.destroyed == 1, "removing the sink destroys its callback");
        captured.messages.clear();
    }
    Foundation::log_warn("cxxapi-log-test after removal");
    check(!saw(captured, LogLevel::Warn, "after removal"), "a removed sink receives nothing more");
    log_remove_sink(sink); // an already removed handle is not an error
    log_remove_sink(123456);

    if (failures != 0) {
        std::fprintf(stderr, "CxxApiLogTest: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("CxxApiLogTest: ok\n");
    return 0;
}
