#pragma once

#include <spdlog/common.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace SFT::Foundation {

    enum class DiagnosticSeverity {
        Error,
        Warning,
        Note,
    };

    // Owned, failure-path-only data used to render compiler-style console diagnostics. Keeping this
    // separate from subsystem error objects lets those remain small `expected` payloads; allocations
    // happen only when a product boundary actually chooses to print a failure.
    struct ConsoleDiagnostic {
        DiagnosticSeverity severity = DiagnosticSeverity::Error;
        std::string code;
        std::string summary;
        std::string context;
        std::string cause_code;
        std::string cause;
        std::string details;
        std::string help;
    };

    [[nodiscard]] constexpr std::string_view diagnostic_severity_name(
        DiagnosticSeverity severity) noexcept {
        switch (severity) {
            case DiagnosticSeverity::Error: return "error";
            case DiagnosticSeverity::Warning: return "warning";
            case DiagnosticSeverity::Note: return "note";
        }
        return "diagnostic";
    }

    namespace Detail {

        inline void append_indented_lines(
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

    } // namespace Detail

    [[nodiscard]] inline std::string format_console_diagnostic(
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

    [[nodiscard]] constexpr spdlog::level::level_enum diagnostic_log_level(
        DiagnosticSeverity severity) noexcept {
        switch (severity) {
            case DiagnosticSeverity::Error: return spdlog::level::err;
            case DiagnosticSeverity::Warning: return spdlog::level::warn;
            case DiagnosticSeverity::Note: return spdlog::level::info;
        }
        return spdlog::level::info;
    }

} // namespace SFT::Foundation
