//! Minimal hot-reload module: the three exports the engine's `HotReloadableModule` looks up.

#[unsafe(no_mangle)]
pub extern "C" fn sturdy_module_register_types(_registry: *mut core::ffi::c_void) {}

#[unsafe(no_mangle)]
pub extern "C" fn sturdy_module_unregister_types(_registry: *mut core::ffi::c_void) {}

#[unsafe(no_mangle)]
pub extern "C" fn sturdy_module_version() -> i32 {
    7
}
