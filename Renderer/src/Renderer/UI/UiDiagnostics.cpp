#include <Renderer/UI/UiDiagnostics.hpp>

#include <algorithm>
#include <format>

namespace SFT::UI {

    std::string_view to_string(UiErrorCode code) noexcept {
        switch (code) {
            case UiErrorCode::None: return "none";
            case UiErrorCode::ElementLimit: return "element-limit";
            case UiErrorCode::ElementConfigLimit: return "element-config-limit";
            case UiErrorCode::TextLineLimit: return "text-line-limit";
            case UiErrorCode::DrawCommandLimit: return "draw-command-limit";
            case UiErrorCode::TextCacheLimit: return "text-cache-limit";
            case UiErrorCode::FixedArenaTooSmall: return "fixed-arena-too-small";
            case UiErrorCode::MemoryLimitReached: return "memory-limit-reached";
            case UiErrorCode::AllocationFailed: return "allocation-failed";
            case UiErrorCode::UnbalancedClose: return "unbalanced-close";
            case UiErrorCode::InvalidArgument: return "invalid-argument";
            case UiErrorCode::DuplicateId: return "duplicate-id";
            case UiErrorCode::FloatingParentMissing: return "floating-parent-missing";
            case UiErrorCode::PercentageOverOne: return "percentage-over-one";
            case UiErrorCode::MeasureFunctionMissing: return "measure-function-missing";
            case UiErrorCode::InternalBoundsViolation: return "internal-bounds-violation";
            case UiErrorCode::TraversalOverflow: return "traversal-overflow";
            case UiErrorCode::SettingIgnored: return "setting-ignored";
            case UiErrorCode::DynamicStringLimit: return "dynamic-string-limit";
            case UiErrorCode::Resized: return "resized";
            case UiErrorCode::Other: return "other";
        }
        return "unknown";
    }

    std::string_view to_string(UiSeverity severity) noexcept {
        switch (severity) {
            case UiSeverity::Info: return "info";
            case UiSeverity::Warning: return "warning";
            case UiSeverity::Error: return "error";
        }
        return "unknown";
    }

    std::string format_report(const UiErrorReport &report, bool include_backtrace) {
        std::string out;
        try {
            out += std::format("UI {} [{}]: {}\n", to_string(report.severity), to_string(report.code), report.message);
            if (report.value != 0 || report.limit != 0) {
                out += std::format("  numbers: {} (limit {})\n", report.value, report.limit);
            }
            if (report.occurrences > 1) {
                out += std::format("  seen {} times, frames {}..{}\n", report.occurrences, report.first_frame, report.last_frame);
            } else {
                out += std::format("  frame {}\n", report.first_frame);
            }
            if (report.foreign_call_site.valid()) {
                out += std::format("  caused by: {}\n", report.foreign_call_site.to_string());
            }
            if (report.api_call_site.valid()) {
                out += std::format("  in UI call: {}\n", report.api_call_site.to_string());
            }
            if (!report.engine_source.empty()) {
                out += std::format("  detected at: {}\n", report.engine_source);
            }
            if (!report.element_path.empty()) {
                out += "  element path: ";
                for (usize i = 0; i < report.element_path.size(); ++i) {
                    out += i == 0 ? "" : " > ";
                    out += report.element_path[i];
                }
                out += '\n';
            }
            if (include_backtrace && !report.backtrace.empty()) {
                out += "  native stack:\n";
                const std::string frames = Foundation::format_backtrace(report.backtrace.resolve());
                usize start = 0;
                while (start < frames.size()) {
                    const usize end = frames.find('\n', start);
                    out += "    ";
                    out.append(frames, start, end == std::string::npos ? std::string::npos : end - start);
                    out += '\n';
                    if (end == std::string::npos) {
                        break;
                    }
                    start = end + 1;
                }
            }
        } catch (...) {
            // Formatting is best effort; keep whatever was built.
        }
        return out;
    }

    namespace {

        [[nodiscard]] bool same_problem(const UiErrorReport &a, const UiErrorReport &b) noexcept {
            return a.code == b.code && a.engine_source == b.engine_source && a.element_path == b.element_path &&
                   a.foreign_call_site.file == b.foreign_call_site.file && a.foreign_call_site.line == b.foreign_call_site.line;
        }

        [[nodiscard]] bool is_power_of_two(u64 value) noexcept { return value != 0 && (value & (value - 1)) == 0; }

        void log_report(const UiErrorReport &report) noexcept {
            const std::string text = format_report(report, report.severity == UiSeverity::Error);
            switch (report.severity) {
                case UiSeverity::Info: Foundation::log_info(text); break;
                case UiSeverity::Warning: Foundation::log_warn(text); break;
                case UiSeverity::Error: Foundation::log_error(text); break;
            }
        }

    } // namespace

    void UiDiagnostics::record(UiErrorReport &&fresh) noexcept {
        try {
            const usize severity_index = std::min<usize>(static_cast<usize>(fresh.severity), 2);
            totals_[severity_index] += 1;
            for (UiErrorReport &existing : reports_) {
                if (!same_problem(existing, fresh)) {
                    continue;
                }
                existing.occurrences += 1;
                existing.last_frame = fresh.last_frame;
                existing.value = fresh.value;
                existing.limit = fresh.limit;
                if (is_power_of_two(existing.occurrences)) {
                    if (fresh.severity >= log_minimum_) {
                        log_report(existing);
                    }
                    if (callback_ && !in_callback_) {
                        in_callback_ = true;
                        callback_(existing);
                        in_callback_ = false;
                    }
                }
                return;
            }
            if (reports_.size() >= max_reports) {
                dropped_ += 1;
                return;
            }
            fresh.occurrences = 1;
            fresh.first_frame = fresh.last_frame;
            reports_.push_back(std::move(fresh));
            const UiErrorReport &stored = reports_.back();
            if (stored.severity >= log_minimum_) {
                log_report(stored);
            }
            if (callback_ && !in_callback_) {
                in_callback_ = true;
                callback_(stored);
                in_callback_ = false;
            }
        } catch (...) {
            in_callback_ = false;
            dropped_ += 1;
        }
    }

    void UiDiagnostics::update_numbers(UiErrorCode code, u32 frame, i64 value, i64 limit) noexcept {
        for (UiErrorReport &existing : reports_) {
            if (existing.code == code && existing.last_frame == frame) {
                existing.value = value;
                existing.limit = limit;
            }
        }
    }

    bool UiDiagnostics::note_repeat(UiErrorCode code, const char *source_file, i32 source_line, u32 frame, i64 value, i64 limit) noexcept {
        for (UiErrorReport &existing : reports_) {
            if (existing.code != code || existing.source_key_line != source_line || existing.source_key_file != source_file ||
                existing.occurrences < 2) {
                continue;
            }
            totals_[std::min<usize>(static_cast<usize>(existing.severity), 2)] += 1;
            existing.occurrences += 1;
            existing.last_frame = frame;
            existing.value = value;
            existing.limit = limit;
            if (is_power_of_two(existing.occurrences)) {
                if (existing.severity >= log_minimum_) {
                    log_report(existing);
                }
                if (callback_ && !in_callback_) {
                    in_callback_ = true;
                    try {
                        callback_(existing);
                    } catch (...) {
                    }
                    in_callback_ = false;
                }
            }
            return true;
        }
        return false;
    }

    std::vector<UiErrorReport> UiDiagnostics::take_reports() noexcept {
        std::vector<UiErrorReport> taken;
        try {
            taken = std::move(reports_);
        } catch (...) {
        }
        reports_.clear();
        totals_[0] = totals_[1] = totals_[2] = 0;
        dropped_ = 0;
        return taken;
    }

    void UiDiagnostics::clear() noexcept {
        reports_.clear();
        totals_[0] = totals_[1] = totals_[2] = 0;
        dropped_ = 0;
    }

    u64 UiDiagnostics::occurrences(UiSeverity minimum) const noexcept {
        // `totals_` counts every occurrence, including repeats and reports that were dropped for lack of room.
        u64 total = 0;
        for (usize i = static_cast<usize>(minimum); i < 3; ++i) {
            total += totals_[i];
        }
        return total;
    }

} // namespace SFT::UI
