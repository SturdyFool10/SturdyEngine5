#pragma once

#include <Foundation/src/Types.hpp>

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

    [[nodiscard]] string human_readable_time(f64 seconds);

    /// Read an entire file into a `string`, **binary** (no newline translation, so it round-trips shader
    /// source and other exact bytes). Returns `nullopt` if the file can't be opened or a read error
    /// occurs — never throws.
    ///
    /// ```cpp
    /// if (auto text = read_file_to_string("Shaders/triangle.slang"))
    ///     compile(*text);
    /// else
    ///     log_error("could not read shader");
    /// ```
    [[nodiscard]] optional<string> read_file_to_string(const fs::path &path);

} // namespace SFT::Foundation
