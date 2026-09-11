#pragma once

#include <Foundation/Foundation.hpp>

#include <filesystem>

namespace SFT::Engine {


    /// Polls a single file's modification time to detect a rebuild, mirroring
    /// `Core::Slang::ShaderWatcher`'s shape (mtime-diffing, no OS file-watch API) for the one file
    /// a `HotReloadableModule` cares about instead of a whole directory tree.
    ///
    /// Like `ShaderWatcher`, this is a passive, synchronous poll — it owns no thread. A caller
    /// wanting non-blocking behavior schedules `poll()` itself (e.g. via `Async::Scheduler`, the
    /// way `Renderer::poll_shader_hot_reload()` drives `ShaderWatcher`); this type just answers
    /// "has the file changed since I last looked."
    class HotReloadWatcher {
      public:
        /// Constructs a watcher for `path`. Establishes a baseline modification time immediately
        /// (if the file exists) so the first `poll()` does not itself report a spurious change.
        explicit HotReloadWatcher(std::filesystem::path path);

        /// Returns the watched path.
        ///
        /// @note This function does not throw exceptions.
        [[nodiscard]] const std::filesystem::path &path() const noexcept {
            return path_;
        }

        /// Checks whether the watched file's modification time has changed since the last
        /// `poll()` (or construction). A missing/inaccessible file (e.g. mid-rebuild, briefly
        /// absent while a linker replaces it) is treated as "no change yet" rather than an
        /// error — the next successful poll after the file reappears will correctly report the
        /// change once, against the last time it was actually observed.
        ///
        /// @return Returns `true` when the file's modification time differs from what was last
        /// observed; `false` otherwise.
        [[nodiscard]] bool poll();

      private:
        std::filesystem::path path_;
        std::filesystem::file_time_type last_write_time_{};
        bool has_last_write_time_ = false;
    };


} // namespace SFT::Engine
