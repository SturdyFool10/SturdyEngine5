#include <Foundation/Utils.hpp>


namespace SFT::Foundation {

    /// Performs the human readable time operation for `Foundation` using the supplied arguments.
    ///
    /// @param seconds `seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    string human_readable_time(f64 seconds) {
        if (!std::isfinite(seconds)) {
            return format("{}s", seconds);
        }

        constexpr auto nanoseconds_per_microsecond = 1'000LL;
        constexpr auto nanoseconds_per_millisecond = 1'000'000LL;
        constexpr auto nanoseconds_per_second = 1'000'000'000LL;
        constexpr auto nanoseconds_per_minute = 60LL * nanoseconds_per_second;
        constexpr auto nanoseconds_per_hour = 60LL * nanoseconds_per_minute;

        const bool negative = seconds < 0.0;
        auto remaining = round<nanoseconds>(duration<f64>{std::abs(seconds)}).count();

        const auto hours = remaining / nanoseconds_per_hour;
        remaining %= nanoseconds_per_hour;
        const auto minutes = remaining / nanoseconds_per_minute;
        remaining %= nanoseconds_per_minute;
        const auto whole_seconds = remaining / nanoseconds_per_second;
        remaining %= nanoseconds_per_second;
        const auto milliseconds = remaining / nanoseconds_per_millisecond;
        remaining %= nanoseconds_per_millisecond;
        const auto microseconds = remaining / nanoseconds_per_microsecond;
        remaining %= nanoseconds_per_microsecond;
        const auto final_nanoseconds = remaining;

        string formatted;
        if (negative) {
            formatted += '-';
        }

        auto append_unit = [&](auto value, const char *suffix) {
            if (value == 0) {
                return;
            }
            if (!formatted.empty() and formatted.back() != '-') {
                formatted += ' ';
            }
            formatted += format("{}{}", value, suffix);
        };

        append_unit(hours, "hr");
        append_unit(minutes, "m");
        append_unit(whole_seconds, "s");
        append_unit(milliseconds, "ms");
        append_unit(microseconds, "us");
        append_unit(final_nanoseconds, "ns");

        if (formatted.empty() or formatted == "-") {
            return "0s";
        }

        return formatted;
    }

    /// Reads file to string from the associated source.
    ///
    /// @param path Filesystem path identifying the target resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    /// @note Normal inability to produce a value is represented by an empty optional.
    optional<string> read_file_to_string(const fs::path &path) {
        ifstream file(path, ios::in | ios::binary);
        if (!file) {
            return nullopt;
        }

        ostringstream contents;
        contents << file.rdbuf();
        if (file.bad()) {
            return nullopt;
        }

        return contents.str();
    }

} // namespace SFT::Foundation

