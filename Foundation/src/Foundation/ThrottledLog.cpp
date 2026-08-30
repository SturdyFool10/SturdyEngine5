#include <Foundation/ThrottledLog.hpp>

#pragma region Imports
#include <mutex>
#include <string>
#include <unordered_map>
#pragma endregion

namespace SFT::Foundation {

    namespace {

        struct GateState {
            std::chrono::steady_clock::time_point last_emit{};
            bool has_emitted = false;
            i64 suppressed_since_emit = 0;
        };

        std::mutex g_throttled_log_mutex;
        std::unordered_map<std::string, GateState> g_throttled_log_gates;

        /// Builds this gate's map key from the call site's own file+line — two different call sites in
        /// the same file never share a key, and the same call site always does, regardless of which
        /// thread or how many times it fires.
        std::string gate_key(const std::source_location &location) {
            return std::string(location.file_name()) + ":" + std::to_string(location.line());
        }

    } // namespace

    /// Decides whether the call site identified by `location` should emit right now — see the
    /// declaration's own doc comment (ThrottledLog.hpp) for the full contract.
    ///
    /// @return Returns `-1` when still inside the throttle window (don't emit), otherwise the number of
    ///         calls suppressed since the last emission (`0` on the very first call from this site).
    /// @note This function does not throw exceptions.
    i64 throttled_log_gate(const std::source_location &location, std::chrono::milliseconds min_interval) noexcept {
        try {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(g_throttled_log_mutex);
            GateState &state = g_throttled_log_gates[gate_key(location)];
            if (!state.has_emitted) {
                state.has_emitted = true;
                state.last_emit = now;
                state.suppressed_since_emit = 0;
                return 0;
            }
            if (now - state.last_emit < min_interval) {
                ++state.suppressed_since_emit;
                return -1;
            }
            const i64 suppressed = state.suppressed_since_emit;
            state.last_emit = now;
            state.suppressed_since_emit = 0;
            return suppressed;
        } catch (...) {
            // Fail open: a broken gate should never be the reason a real error goes unlogged.
            return 0;
        }
    }

    /// Clears all throttle-gate state — see the declaration's own doc comment (ThrottledLog.hpp).
    ///
    /// @note This function does not throw exceptions.
    void reset_throttled_log_gates() noexcept {
        try {
            std::lock_guard<std::mutex> lock(g_throttled_log_mutex);
            g_throttled_log_gates.clear();
        } catch (...) {
        }
    }

    /// Formats and emits one throttled log line — see the declaration's own doc comment
    /// (ThrottledLog.hpp) for the full contract.
    ///
    /// @note This function does not throw exceptions.
    void emit_throttled_log(spdlog::level::level_enum level, const std::source_location &location, i64 suppressed,
                            string_view message) noexcept {
        try {
            if (suppressed > 0) {
                log(level, "{} [{}:{} in {}] (+{} suppressed since last)", message, location.file_name(),
                    location.line(), location.function_name(), suppressed);
            } else {
                log(level, "{} [{}:{} in {}]", message, location.file_name(), location.line(), location.function_name());
            }
        } catch (...) {
        }
    }

} // namespace SFT::Foundation
