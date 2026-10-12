#include <CxxApi/Log.hpp>

#include <string_view>

#include <Foundation/Foundation.hpp>

#include <CxxApi/Error.hpp>

namespace SFT::CxxApi {

    namespace {
        static_assert(static_cast<int>(LogLevel::Critical) == static_cast<int>(Foundation::LogLevel::Critical));
        static_assert(static_cast<int>(LogLevel::Warn) == static_cast<int>(Foundation::LogLevel::Warn));
    } // namespace

    std::uint64_t log_add_sink(std::unique_ptr<LogSinkCallback> callback) {
        if (!callback) {
            fail("log_add_sink needs a callback");
        }
        // A shared pointer, so a message being delivered on another thread keeps the callback alive even if the sink is removed
        // at the same moment.
        std::shared_ptr<LogSinkCallback> shared = std::move(callback);
        const Foundation::LogSinkId id = Foundation::add_log_sink([shared](Foundation::LogLevel level, std::string_view message) {
            shared->on_message(static_cast<LogLevel>(level), message.data(), message.size());
        });
        return id.value;
    }

    void log_remove_sink(std::uint64_t sink) noexcept { Foundation::remove_log_sink(Foundation::LogSinkId{sink}); }

} // namespace SFT::CxxApi
