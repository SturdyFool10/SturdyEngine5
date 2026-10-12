//! Raw `cxx` bindings for the engine's hot-reload support (`Engine/HotReloadableModule.hpp`,
//! `Engine/HotReloadWatcher.hpp`): loading a shared library that exports the
//! `sturdy_module_register_types` / `sturdy_module_unregister_types` / `sturdy_module_version`
//! entry points (see `SFT_HOT_RELOAD_MODULE_EXPORTS`) against the process-wide reflection
//! registry, swapping it for a rebuilt copy, and polling a file for modification.
//!
//! Standalone: neither type needs an `Engine`; modules register into
//! `Reflection::TypeRegistry::instance()`, the same registry `sturdy::reflection` uses.

#[cxx::bridge(namespace = "sturdy_rs::hot_reload")]
pub mod ffi {
    /// Mirrors `SFT::Engine::HotReloadErrorCode`.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum HotReloadErrorCode {
        LibraryLoadFailed,
        MissingEntryPoint,
    }

    #[derive(Debug, Clone)]
    struct HotReloadStatus {
        ok: bool,
        /// Meaningful only when `!ok`.
        code: HotReloadErrorCode,
        message: String,
    }

    unsafe extern "C++" {
        include!("sturdy_rs/hot_reload.hpp");

        /// `SFT::Engine::HotReloadableModule`.
        type HotModule;
        /// `SFT::Engine::HotReloadWatcher`.
        type FileWatcher;

        fn hot_module_new() -> UniquePtr<HotModule>;
        /// Loads `path` and registers its reflected types. A failed load leaves the module as it
        /// was (still unloaded, or still holding the previous library).
        fn hot_module_load(module: Pin<&mut HotModule>, path: &str) -> HotReloadStatus;
        /// Unregisters the module's types and unloads it. No-op if nothing is loaded.
        fn hot_module_unload(module: Pin<&mut HotModule>);
        /// Unload, then load `path`.
        fn hot_module_reload(module: Pin<&mut HotModule>, path: &str) -> HotReloadStatus;
        fn hot_module_is_loaded(module: &HotModule) -> bool;
        /// The module's own `sturdy_module_version()`, or -1 when nothing is loaded.
        fn hot_module_version(module: &HotModule) -> i32;

        fn file_watcher_new(path: &str) -> UniquePtr<FileWatcher>;
        /// True once each time the file's modification time changed since the last poll. A file
        /// that doesn't exist yet reports false until it does.
        fn file_watcher_poll(watcher: Pin<&mut FileWatcher>) -> bool;
    }
}
