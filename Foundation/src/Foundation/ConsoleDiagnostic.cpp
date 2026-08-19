#include <Foundation/ConsoleDiagnostic.hpp>


namespace SFT::Foundation::Detail {

    /// Appends the supplied value or range to the current contents.
    ///
    /// @param output `output` value used by the operation.
    /// @param text Text consumed by the operation.
    /// @param first_prefix `first_prefix` value used by the operation.
    /// @param continuation_prefix `continuation_prefix` value used by the operation.
    ///
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    void append_indented_lines(
        std::string &output,
        std::string_view text,
        std::string_view first_prefix,
        std::string_view continuation_prefix) {
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        if (text.empty()) {
            return;
        }

        std::string_view prefix = first_prefix;
        std::size_t offset = 0;
        while (offset <= text.size()) {
            const std::size_t newline = text.find('\n', offset);
            const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
            const std::size_t content_end = end > offset && text[end - 1] == '\r' ? end - 1 : end;
            output.append(prefix);
            output.append(text.substr(offset, content_end - offset));
            if (newline == std::string_view::npos) {
                break;
            }
            output.push_back('\n');
            prefix = continuation_prefix;
            offset = newline + 1;
        }
    }

} // namespace SFT::Foundation::Detail

namespace SFT::Foundation {

    /// Formats console diagnostic using the supplied arguments and current state.
    ///
    /// @param diagnostic `diagnostic` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    std::string format_console_diagnostic(
        const ConsoleDiagnostic &diagnostic) {
        std::string output;
        output.reserve(
            diagnostic.summary.size() + diagnostic.context.size() + diagnostic.cause.size() +
            diagnostic.details.size() + diagnostic.help.size() + 96);

        output.append(diagnostic_severity_name(diagnostic.severity));
        if (!diagnostic.code.empty()) {
            output.push_back('[');
            output.append(diagnostic.code);
            output.push_back(']');
        }
        output.append(": ");
        output.append(diagnostic.summary);

        if (!diagnostic.context.empty()) {
            output.append("\n  --> ");
            output.append(diagnostic.context);
        }

        if (!diagnostic.cause.empty() || !diagnostic.details.empty() || !diagnostic.help.empty()) {
            output.append("\n   |");
        }
        if (!diagnostic.cause.empty()) {
            output.append("\n   = cause");
            if (!diagnostic.cause_code.empty()) {
                output.push_back('[');
                output.append(diagnostic.cause_code);
                output.push_back(']');
            }
            output.append(": ");
            Detail::append_indented_lines(output, diagnostic.cause, {}, "     ");
        }
        if (!diagnostic.details.empty()) {
            output.push_back('\n');
            Detail::append_indented_lines(output, diagnostic.details, "   | ", "   | ");
        }
        if (!diagnostic.help.empty()) {
            output.append("\n   = help: ");
            Detail::append_indented_lines(output, diagnostic.help, {}, "     ");
        }
        return output;
    }

} // namespace SFT::Foundation

