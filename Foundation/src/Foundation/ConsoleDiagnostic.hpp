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

    /// Returns a human-readable name for the supplied diagnostic severity value.
    ///
    /// @param severity `severity` value used by the operation.
    ///
    /// @return Returns a non-owning view of the underlying data; the view remains valid only while that storage is not invalidated.
    /// @note This function does not throw exceptions.
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
            std::string_view continuation_prefix);

    } // namespace Detail

    /// Formats console diagnostic using the supplied arguments and current state.
    ///
    /// @param diagnostic `diagnostic` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
    [[nodiscard]] std::string format_console_diagnostic(
        const ConsoleDiagnostic &diagnostic);

    /// Performs the diagnostic log level operation for `Foundation` using the supplied arguments.
    ///
    /// @param severity `severity` value used by the operation.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
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
