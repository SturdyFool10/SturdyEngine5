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
