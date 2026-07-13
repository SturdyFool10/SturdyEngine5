module;

#pragma region Imports
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#pragma endregion

export module Sturdy.Core:ShaderWatcher;

import Sturdy.Foundation;
import :ShaderTypes;

using std::error_code;
using std::string;
using std::unordered_map;
using std::vector;

namespace fs = std::filesystem;

export namespace SFT::Core::Slang {

    // ─────────────────────────────────────────────────────────────────────────────────────────────
    //  Shader hot-reload: detect edited `.slang` files so a running engine can recompile them.
    //
    //  A dev-time convenience — mtime polling, not inotify/kqueue. The engine calls `poll()` once per
    //  frame (cheap: a recursive directory stat) and gets back the list of `.slang` files whose
    //  modification time changed since the previous poll, or that appeared since. The actual recompile +
    //  reflection-compatibility check + safe resource swap live above this, in the renderer — this class
    //  only answers "what changed on disk." See plans/shader-variants-and-hot-reload.md.
    // ─────────────────────────────────────────────────────────────────────────────────────────────

    // One reported change: which `.slang` file, and whether it is newly seen (`added`) or a
    // modification of a file we were already tracking.
    struct ShaderChange {
        string path;      // filesystem path to the changed `.slang` file
        bool added = false; // true if first time seen this poll cycle, false if an mtime change
    };

    // Recursively watches a directory tree for edited `.slang` files by polling modification times.
    //
    // Construct it over the same `Shaders/` tree `discover_shaders()` scans. By default it *primes*
    // itself (records every file's current mtime) so the first `poll()` reports nothing but genuine
    // edits made after construction. Pass `prime = false` to have the first `poll()` report every file
    // as `added` — handy if the caller wants a single code path that compiles everything on the first
    // tick and reloads on later ones.
    class ShaderWatcher {
      public:
        explicit ShaderWatcher(fs::path directory, bool prime = true) : directory_(std::move(directory)) {
            if (prime) {
                error_code ec;
                scan(directory_, [this](const fs::path &path, fs::file_time_type mtime) {
                    mtimes_.emplace(path.string(), mtime);
                });
            }
        }

        [[nodiscard]] const fs::path &directory() const noexcept { return directory_; }
        [[nodiscard]] usize tracked_count() const noexcept { return mtimes_.size(); }

        // Re-stat the tree and return everything whose mtime changed or that appeared since the last
        // poll. Deleted files are dropped from tracking silently (a shader that no longer exists can't be
        // reloaded). A missing/unreadable directory yields an empty result rather than erroring — this is
        // a best-effort dev feature, never a hard failure in the frame loop.
        [[nodiscard]] vector<ShaderChange> poll() {
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

      private:
        // Visit every regular `.slang` file under `root` (recursively), calling `visit(path, mtime)`.
        // Swallows filesystem errors per the best-effort contract; a permission-denied subtree is skipped
        // rather than aborting the walk.
        template <typename Visit>
        static void scan(const fs::path &root, Visit &&visit) {
            error_code ec;
            if (!fs::is_directory(root, ec) || ec) {
                return;
            }
            fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec)) {
                const fs::directory_entry &entry = *it;
                if (!entry.is_regular_file(ec) || ec || entry.path().extension() != shader_file_extension_) {
                    continue;
                }
                error_code time_ec;
                const fs::file_time_type mtime = fs::last_write_time(entry.path(), time_ec);
                if (time_ec) {
                    continue;
                }
                visit(entry.path(), mtime);
            }
        }

        // Kept local (rather than importing :ShaderDiscovery) so the watcher depends only on std +
        // :ShaderTypes; it is the same `.slang` extension `discover_shaders()` uses.
        static constexpr std::string_view shader_file_extension_ = ".slang";

        fs::path directory_;
        unordered_map<string, fs::file_time_type> mtimes_;
    };

} // namespace SFT::Core::Slang
