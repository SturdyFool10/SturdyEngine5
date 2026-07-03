module;

#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>

export module Sturdy.Foundation:Utils;

using std::ifstream;
using std::ios;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::string;

namespace fs = std::filesystem;

export namespace SFT::Foundation {

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
