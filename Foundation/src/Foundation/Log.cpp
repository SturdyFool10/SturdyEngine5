#include <Foundation/Log.hpp>


namespace SFT::Foundation {

    /// Logs the supplied or associated value/state using the supplied arguments and current state.
    ///
    /// @param level `level` value used by the operation.
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log(spdlog::level::level_enum level, string_view message) noexcept {
        try {
            ::spdlog::log(level, "{}", message);
        } catch (...) {
        }
    }

    /// Logs trace using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_trace(string_view message) noexcept { log(spdlog::level::trace, message); }

    /// Logs debug using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_debug(string_view message) noexcept { log(spdlog::level::debug, message); }

    /// Logs info using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_info(string_view message) noexcept { log(spdlog::level::info, message); }

    /// Logs warn using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_warn(string_view message) noexcept { log(spdlog::level::warn, message); }

    /// Logs error using the supplied arguments and current state.
    ///
    /// @param message Text consumed by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_error(string_view message) noexcept { log(spdlog::level::err, message); }

    /// Logs diagnostic using the supplied arguments and current state.
    ///
    /// @param diagnostic `diagnostic` value used by the operation.
    ///
    /// @note This function does not throw exceptions.
    void log_diagnostic(const ConsoleDiagnostic &diagnostic) noexcept {
        log(diagnostic_log_level(diagnostic.severity), format_console_diagnostic(diagnostic));
    }

    /// Flushes logs.
    ///
    /// @note This function does not throw exceptions.
    void flush_logs() noexcept {
        try {
            if (const auto logger = ::spdlog::default_logger()) {
                logger->flush();
            }
        } catch (...) {
        }
    }

    /// Initializes file logging for use.
    ///
    /// @param log_file_path Filesystem path identifying the target resource.
    /// @param max_file_size_bytes Size of the relevant data in bytes.
    /// @param max_rotated_files `max_rotated_files` value used by the operation.
    ///
    /// @return Returns the boolean result of the operation.
    /// @note This function does not throw exceptions.
    bool init_file_logging(
        const std::filesystem::path &log_file_path,
        std::size_t max_file_size_bytes,
        std::size_t max_rotated_files) noexcept {
        try {
            std::error_code ec;
            std::filesystem::create_directories(log_file_path.parent_path(), ec);

            const auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                log_file_path.string(), max_file_size_bytes, max_rotated_files);
            file_sink->set_level(spdlog::level::trace);

            const auto logger = ::spdlog::default_logger();
            if (!logger) {
                return false;
            }
            logger->sinks().push_back(file_sink);
            logger->flush_on(spdlog::level::warn);
            return true;
        } catch (...) {
            return false;
        }
    }

} // namespace SFT::Foundation

