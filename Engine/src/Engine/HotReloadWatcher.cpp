#include <Engine/HotReloadWatcher.hpp>

#include <system_error>

namespace SFT::Engine {

    HotReloadWatcher::HotReloadWatcher(std::filesystem::path path) : path_(std::move(path)) {
        std::error_code error;
        const auto write_time = std::filesystem::last_write_time(path_, error);
        if (!error) {
            last_write_time_ = write_time;
            has_last_write_time_ = true;
        }
    }

    bool HotReloadWatcher::poll() {
        std::error_code error;
        const auto write_time = std::filesystem::last_write_time(path_, error);
        if (error) {
            return false;
        }
        if (!has_last_write_time_) {
            last_write_time_ = write_time;
            has_last_write_time_ = true;
            return false;
        }
        if (write_time == last_write_time_) {
            return false;
        }
        last_write_time_ = write_time;
        return true;
    }

} // namespace SFT::Engine
