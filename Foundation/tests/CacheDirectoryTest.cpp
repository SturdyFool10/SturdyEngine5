#include <Foundation/CacheDirectory.hpp>

#include <iostream>

int main() {
    using namespace SFT::Foundation;
    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        if (!ok) {
            std::cerr << "FAILED: " << what << '\n';
            ++failures;
        }
    };

    const std::filesystem::path legacy{".cache/things"};
    set_cache_root({});
    if (!cache_root()) { // STURDY_CACHE_DIR unset
        check(cache_subdirectory("things", legacy) == legacy, "without a root the legacy location is kept");
    }
    set_cache_root("/tmp/sturdy-cache-root");
    check(cache_root() && *cache_root() == "/tmp/sturdy-cache-root", "the override is reported");
    check(cache_subdirectory("things", legacy) == std::filesystem::path{"/tmp/sturdy-cache-root/things"}, "the override relocates the cache");
    set_cache_root({});
    return failures == 0 ? 0 : 1;
}
