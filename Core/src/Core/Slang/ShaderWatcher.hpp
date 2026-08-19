#pragma once

#include <Foundation/Foundation.hpp>

#pragma region Imports
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>
#pragma endregion

#include <Core/Slang/ShaderTypes.hpp>

using std::error_code;
using std::string;
using std::unordered_map;
using std::vector;

namespace fs = std::filesystem;

namespace SFT::Core::Slang {


    struct ShaderChange {
        string path;
        bool added = false;
    };


    class ShaderWatcher {
      public:
        /// Constructs a `ShaderWatcher` from the supplied initialization values.
        ///
        /// @param directory `directory` value used by the operation.
        /// @param prime `prime` value used by the operation.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        explicit ShaderWatcher(fs::path directory, bool prime = true);

        /// Returns the current or globally available directory value.
        ///
        /// @return Returns a read-only reference to the requested state; the reference is tied to the lifetime of its owning object.
        /// @note This function does not throw exceptions.
        [[nodiscard]] const fs::path &directory() const noexcept;
        /// Returns the tracked count for this `ShaderWatcher`.
        ///
        /// @return Returns the current tracked count value.
        /// @note This function does not throw exceptions.
        [[nodiscard]] usize tracked_count() const noexcept;


        /// Polls the associated runtime source for available work or state changes.
        ///
        /// @return Returns the current poll value.
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
        [[nodiscard]] vector<ShaderChange> poll();

      private:


        /// Performs the scan operation for `ShaderWatcher` using the supplied arguments.
        ///
        /// @note This function has no separate failure status; exceptions raised by operations it invokes propagate to the caller.
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


        static constexpr std::string_view shader_file_extension_ = ".slang";

        fs::path directory_;
        unordered_map<string, fs::file_time_type> mtimes_;
    };

} // namespace SFT::Core::Slang
