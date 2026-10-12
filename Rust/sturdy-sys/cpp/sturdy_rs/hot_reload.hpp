// Rust <-> SturdyEngine 5 shim for HotReloadableModule / HotReloadWatcher. See src/hot_reload.rs.
#pragma once

#include <cstdint>
#include <memory>

#include <rust/cxx.h>

#include <Engine/HotReloadWatcher.hpp>
#include <Engine/HotReloadableModule.hpp>

namespace sturdy_rs::hot_reload {

    enum class HotReloadErrorCode : std::uint8_t;
    struct HotReloadStatus;

    using HotModule = SFT::Engine::HotReloadableModule;
    using FileWatcher = SFT::Engine::HotReloadWatcher;

    std::unique_ptr<HotModule> hot_module_new();
    HotReloadStatus hot_module_load(HotModule &module, rust::Str path);
    void hot_module_unload(HotModule &module);
    HotReloadStatus hot_module_reload(HotModule &module, rust::Str path);
    bool hot_module_is_loaded(const HotModule &module) noexcept;
    std::int32_t hot_module_version(const HotModule &module) noexcept;

    std::unique_ptr<FileWatcher> file_watcher_new(rust::Str path);
    bool file_watcher_poll(FileWatcher &watcher);

} // namespace sturdy_rs::hot_reload
