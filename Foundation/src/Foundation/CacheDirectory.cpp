#include <Foundation/CacheDirectory.hpp>

#include <cstdlib>
#include <mutex>

namespace SFT::Foundation {

    namespace {

        std::mutex &override_mutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::filesystem::path &override_root() {
            static std::filesystem::path root;
            return root;
        }

    } // namespace

    void set_cache_root(std::filesystem::path root) {
        std::lock_guard lock(override_mutex());
        override_root() = std::move(root);
    }

    std::optional<std::filesystem::path> cache_root() {
        {
            std::lock_guard lock(override_mutex());
            if (!override_root().empty()) {
                return override_root();
            }
        }
        if (const char *environment = std::getenv("STURDY_CACHE_DIR"); environment != nullptr && environment[0] != '\0') {
            return std::filesystem::path{environment};
        }
        return std::nullopt;
    }

    std::filesystem::path cache_subdirectory(const char *name, const std::filesystem::path &legacy_default) {
        if (auto root = cache_root()) {
            return *root / name;
        }
        return legacy_default;
    }

} // namespace SFT::Foundation
