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

    /// Performs the human readable time operation using the supplied arguments.
    ///
    /// @param seconds `seconds` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] string human_readable_time(f64 seconds);


    /// Reads file to string from the associated source.
    ///
    /// @param path Filesystem path identifying the target resource.
    ///
    /// @return Returns an engaged optional containing the result on success; returns `std::nullopt` when no result can be produced.
    [[nodiscard]] optional<string> read_file_to_string(const fs::path &path);

} // namespace SFT::Foundation
