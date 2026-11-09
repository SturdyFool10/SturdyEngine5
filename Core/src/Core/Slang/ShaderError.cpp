#include <Core/Slang/ShaderError.hpp>

namespace SFT::Core::Slang {

/// Creates an error result describing the supplied shader failure.
///
/// @param code `code` value used by the operation.
/// @param message Text consumed by the operation.
/// @param diagnostics `diagnostics` value used by the operation.
///
/// @return Returns the value alternative on success; the error alternative describes why the operation failed.
/// @note Normal failures are returned through the type-specific error/status state; invalid input/state and underlying backend or resource failures are reported there when detected.
namespace {

    /// The first few lines of a compiler's diagnostics, capped: enough to say what went wrong when only `message` is shown.
    [[nodiscard]] string diagnostics_headline(const string &diagnostics) {
        constexpr usize max_lines = 3;
        constexpr usize max_bytes = 480;
        string headline;
        usize lines = 0;
        usize begin = 0;
        while (begin < diagnostics.size() && lines < max_lines) {
            usize end = diagnostics.find('\n', begin);
            if (end == string::npos) {
                end = diagnostics.size();
            }
            string line = diagnostics.substr(begin, end - begin);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                headline += lines == 0 ? "" : " | ";
                headline += line;
                ++lines;
            }
            begin = end + 1;
        }
        if (headline.size() > max_bytes) {
            headline.resize(max_bytes);
            headline += "...";
        } else if (begin < diagnostics.size()) {
            headline += " | ...";
        }
        return headline;
    }

} // namespace

unexpected<ShaderError> shader_error(ShaderErrorCode code, string message, string diagnostics) {
        // Callers differ in whether they print `diagnostics` next to `message`, and several print only `message`. Putting the compiler's own
        // first lines into the message means no layer can report "compilation failed" without saying what failed.
        if (!diagnostics.empty() && message.find("Compiler said:") == string::npos) {
            message += " Compiler said: " + diagnostics_headline(diagnostics);
        }
        // Record where the failure was found: by the time the error value reaches whoever reports it, this stack is long gone.
        Foundation::note_trace_origin();
        return unexpected(ShaderError{code, std::move(message), std::move(diagnostics)});
    }

} // namespace SFT::Core::Slang
