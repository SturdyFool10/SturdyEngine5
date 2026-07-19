#pragma once

#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>



using SFT::Foundation::f64;
using std::chrono::duration;
using std::chrono::nanoseconds;
using std::chrono::round;
using std::format;
using std::ifstream;
using std::ios;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::string;

namespace fs = std::filesystem;

namespace SFT::Foundation {

    [[nodiscard]] inline string human_readable_time(f64 seconds) {
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

    // Read an entire file into a `string`, **binary** (no newline translation, so it round-trips shader
    // source and other exact bytes). Returns `nullopt` if the file can't be opened or a read error
    // occurs — never throws.
    //
    // ```cpp
    // if (auto text = read_file_to_string("Shaders/triangle.slang"))
    //     compile(*text);
    // else
    //     log_error("could not read shader");
    // ```
    [[nodiscard]] inline optional<string> read_file_to_string(const fs::path &path) {
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
