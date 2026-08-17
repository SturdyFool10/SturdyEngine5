#include "ShaderWatcher.hpp"

namespace SFT::Core::Slang {

/// Performs the shader watcher operation for `Slang` using the supplied arguments.
///
/// @param directory `directory` value used by the operation.
/// @param prime `prime` value used by the operation.
///
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
ShaderWatcher::ShaderWatcher(fs::path directory, bool prime) : directory_(std::move(directory)) {
            if (prime) {
                error_code ec;
                scan(directory_, [this](const fs::path &path, fs::file_time_type mtime) {
                    mtimes_.emplace(path.string(), mtime);
                });
            }
        }

/// Returns the current or globally available directory value.
///
/// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
/// @note This function does not throw exceptions.
[[nodiscard]] const fs::path &ShaderWatcher::directory() const noexcept { return directory_; }

/// Returns the tracked count for this `Slang`.
///
/// @return Returns the current tracked count value.
/// @note This function does not throw exceptions.
[[nodiscard]] usize ShaderWatcher::tracked_count() const noexcept { return mtimes_.size(); }

/// Polls the associated runtime source for available work or state changes.
///
/// @return Returns the current poll value.
/// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
[[nodiscard]] vector<ShaderChange> ShaderWatcher::poll() {
            vector<ShaderChange> changes;
            unordered_map<string, fs::file_time_type> next;

            scan(directory_, [&](const fs::path &path, fs::file_time_type mtime) {
                const string key = path.string();
                next.emplace(key, mtime);
                const auto previous = mtimes_.find(key);
                if (previous == mtimes_.end()) {
                    changes.push_back(ShaderChange{.path = key, .added = true});
                } else if (previous->second != mtime) {
                    changes.push_back(ShaderChange{.path = key, .added = false});
                }
            });

            mtimes_ = std::move(next);
            return changes;
        }

} // namespace SFT::Core::Slang
