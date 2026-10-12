//! Hot-reloadable reflection modules and file watching.
//!
//! A *module* is a shared library exporting three C symbols — `sturdy_module_register_types(
//! TypeRegistry*)`, `sturdy_module_unregister_types(TypeRegistry*)` and `sturdy_module_version()
//! -> i32` (the engine's `SFT_HOT_RELOAD_MODULE_EXPORTS` macro declares them). Loading one runs its
//! register function against the process-wide reflection registry, so its types show up in
//! [`crate::reflection`] (`find_type`, `fields`, ...); unloading unregisters them. Pair a
//! [`HotModule`] with a [`FileWatcher`] to reload a rebuilt library while the game keeps running.
//!
//! The register/unregister functions receive a pointer to the engine's C++ registry, so a module
//! is authored in C++ against the engine headers. A Rust `cdylib` can export no-op/version-only
//! entry points, but cannot register types itself without linking its own copy of the engine.

use std::path::Path;

use cxx::UniquePtr;
use sturdy_sys::hot_reload::ffi;

/// Why a module failed to load.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HotReloadError {
    /// The dynamic loader rejected the file; carries its message.
    LibraryLoadFailed(String),
    /// The library loaded but lacks one of the three required exports.
    MissingEntryPoint(String),
}

impl core::fmt::Display for HotReloadError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::LibraryLoadFailed(m) => write!(f, "failed to load module library: {m}"),
            Self::MissingEntryPoint(m) => write!(f, "module is missing an entry point: {m}"),
        }
    }
}

impl std::error::Error for HotReloadError {}

fn status(s: ffi::HotReloadStatus) -> Result<(), HotReloadError> {
    if s.ok {
        Ok(())
    } else if s.code == ffi::HotReloadErrorCode::MissingEntryPoint {
        Err(HotReloadError::MissingEntryPoint(s.message))
    } else {
        Err(HotReloadError::LibraryLoadFailed(s.message))
    }
}

/// One loaded (or not) hot-reloadable module. Unloads — unregistering its types — on drop.
pub struct HotModule {
    inner: UniquePtr<ffi::HotModule>,
}

impl Default for HotModule {
    fn default() -> Self {
        Self::new()
    }
}

impl HotModule {
    /// A module with nothing loaded.
    pub fn new() -> Self {
        Self { inner: ffi::hot_module_new() }
    }

    /// Loads the library at `path` and registers its reflected types. On failure the module is
    /// left as it was.
    pub fn load(&mut self, path: impl AsRef<Path>) -> Result<(), HotReloadError> {
        status(ffi::hot_module_load(self.inner.pin_mut(), &path.as_ref().to_string_lossy()))
    }

    /// Unregisters the module's types and unloads the library. No-op if nothing is loaded.
    pub fn unload(&mut self) {
        ffi::hot_module_unload(self.inner.pin_mut());
    }

    /// Unloads the current library (if any), then loads `path`. Note that the previous library
    /// is gone even if the new one fails to load.
    pub fn reload(&mut self, path: impl AsRef<Path>) -> Result<(), HotReloadError> {
        status(ffi::hot_module_reload(self.inner.pin_mut(), &path.as_ref().to_string_lossy()))
    }

    pub fn is_loaded(&self) -> bool {
        ffi::hot_module_is_loaded(&self.inner)
    }

    /// The loaded module's own `sturdy_module_version()`, or `None` when nothing is loaded.
    pub fn version(&self) -> Option<i32> {
        self.is_loaded().then(|| ffi::hot_module_version(&self.inner))
    }
}

/// Polls a file's modification time.
pub struct FileWatcher {
    inner: UniquePtr<ffi::FileWatcher>,
}

impl FileWatcher {
    /// Starts watching `path`; the file need not exist yet.
    pub fn new(path: impl AsRef<Path>) -> Self {
        Self { inner: ffi::file_watcher_new(&path.as_ref().to_string_lossy()) }
    }

    /// True once each time the file changed since the previous poll (or since it appeared).
    pub fn poll(&mut self) -> bool {
        ffi::file_watcher_poll(self.inner.pin_mut())
    }
}
