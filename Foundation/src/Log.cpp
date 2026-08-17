#include <Foundation/src/Log.hpp>


namespace SFT::Foundation {

    void log(spdlog::level::level_enum level, string_view message) noexcept {
        try {
            ::spdlog::log(level, "{}", message);
        } catch (...) {
        }
    }

    void log_trace(string_view message) noexcept { log(spdlog::level::trace, message); }

    void log_debug(string_view message) noexcept { log(spdlog::level::debug, message); }

    void log_info(string_view message) noexcept { log(spdlog::level::info, message); }

    void log_warn(string_view message) noexcept { log(spdlog::level::warn, message); }

    void log_error(string_view message) noexcept { log(spdlog::level::err, message); }

    void log_diagnostic(const ConsoleDiagnostic &diagnostic) noexcept {
        log(diagnostic_log_level(diagnostic.severity), format_console_diagnostic(diagnostic));
    }

    void flush_logs() noexcept {
        try {
            if (const auto logger = ::spdlog::default_logger()) {
                logger->flush();
            }
        } catch (...) {
        }
    }

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

