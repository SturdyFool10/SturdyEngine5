#include <Foundation/LogSink.hpp>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace SFT::Foundation {

    namespace {

        /// Converts an spdlog severity to this API's own `LogLevel`.
        ///
        /// @param level spdlog's own level value.
        ///
        /// @return The matching `LogLevel`; `off`/unrecognized values fall back to `Info` rather
        ///         than reaching an FFI consumer as an out-of-range enum value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] LogLevel to_log_level(spdlog::level::level_enum level) noexcept {
            switch (level) {
                case spdlog::level::trace: return LogLevel::Trace;
                case spdlog::level::debug: return LogLevel::Debug;
                case spdlog::level::info: return LogLevel::Info;
                case spdlog::level::warn: return LogLevel::Warn;
                case spdlog::level::err: return LogLevel::Error;
                case spdlog::level::critical: return LogLevel::Critical;
                case spdlog::level::off:
                case spdlog::level::n_levels:
                default:
                    return LogLevel::Info;
            }
        }

        /// An spdlog sink that fans every message out to a dynamic set of registered
        /// `LogSinkCallback`s, rather than writing anywhere itself. `base_sink<std::mutex>`
        /// already serializes `sink_it_`/`flush_` against spdlog's own concurrent loggers; the
        /// callback registry gets its own separate mutex since callbacks can be added/removed
        /// from unrelated threads at any time, independent of an in-flight log call.
        class CallbackFanoutSink final : public spdlog::sinks::base_sink<std::mutex> {
          public:
            /// Registers `callback`, returning the id `remove` needs to undo it.
            ///
            /// @param callback Function to invoke per message. Must be non-empty.
            ///
            /// @return The new registration's id, or 0 if `callback` was empty.
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            [[nodiscard]] u64 add(LogSinkCallback callback) {
                if (!callback) {
                    return 0;
                }
                const std::lock_guard<std::mutex> lock(registry_mutex_);
                const u64 id = next_id_++;
                callbacks_.emplace(id, std::move(callback));
                return id;
            }

            /// Unregisters the callback added under `id`, if still present.
            ///
            /// @param id Id returned by a prior `add` call.
            ///
            /// @note This function does not throw exceptions.
            void remove(u64 id) noexcept {
                try {
                    const std::lock_guard<std::mutex> lock(registry_mutex_);
                    callbacks_.erase(id);
                } catch (...) {
                }
            }

          protected:
            /// Performs the sink it operation for `CallbackFanoutSink` using the supplied arguments.
            ///
            /// @param msg `msg` value used by the operation.
            ///
            /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
            void sink_it_(const spdlog::details::log_msg &msg) override {
                const LogLevel level = to_log_level(msg.level);
                const std::string_view message{msg.payload.data(), msg.payload.size()};
                const std::lock_guard<std::mutex> lock(registry_mutex_);
                for (const auto &[id, callback] : callbacks_) {
                    // One misbehaving foreign callback must not take down every other sink
                    // (including the engine's own console/file logging) or crash across the FFI
                    // boundary.
                    try {
                        callback(level, message);
                    } catch (...) {
                    }
                }
            }

            /// Flushes the sink identified by the supplied parameters.
            ///
            /// @note This function does not throw exceptions.
            void flush_() override {}

          private:
            std::mutex registry_mutex_;
            std::unordered_map<u64, LogSinkCallback> callbacks_;
            u64 next_id_ = 1;
        };

        /// Returns the process-wide fanout sink, creating and registering it with spdlog's
        /// default logger on first use.
        ///
        /// @return Returns a reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] std::shared_ptr<CallbackFanoutSink> &fanout_sink() {
            static std::shared_ptr<CallbackFanoutSink> sink = [] {
                auto created = std::make_shared<CallbackFanoutSink>();
                created->set_level(spdlog::level::trace);
                if (const auto logger = spdlog::default_logger()) {
                    logger->sinks().push_back(created);
                }
                return created;
            }();
            return sink;
        }

    } // namespace

    LogSinkId add_log_sink(LogSinkCallback callback) {
        const u64 id = fanout_sink()->add(std::move(callback));
        return LogSinkId{id};
    }

    void remove_log_sink(LogSinkId id) noexcept {
        if (!id.is_valid()) {
            return;
        }
        fanout_sink()->remove(id.value);
    }

} // namespace SFT::Foundation
