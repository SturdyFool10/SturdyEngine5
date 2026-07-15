#include "ShaderWatcher.hpp"

namespace SFT::Core::Slang {

ShaderWatcher::ShaderWatcher(fs::path directory, bool prime) : directory_(std::move(directory)) {
            if (prime) {
                error_code ec;
                scan(directory_, [this](const fs::path &path, fs::file_time_type mtime) {
                    mtimes_.emplace(path.string(), mtime);
                });
            }
        }

[[nodiscard]] const fs::path &ShaderWatcher::directory() const noexcept { return directory_; }

[[nodiscard]] usize ShaderWatcher::tracked_count() const noexcept { return mtimes_.size(); }

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
