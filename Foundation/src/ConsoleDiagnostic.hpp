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

    /// Owned, failure-path-only data used to render compiler-style console diagnostics. Keeping this
    /// separate from subsystem error objects lets those remain small `expected` payloads; allocations
    /// happen only when a product boundary actually chooses to print a failure.
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

        void append_indented_lines(
            std::string &output,
            std::string_view text,
            std::string_view first_prefix,
            std::string_view continuation_prefix);

    } // namespace Detail

    [[nodiscard]] std::string format_console_diagnostic(
        const ConsoleDiagnostic &diagnostic);

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
