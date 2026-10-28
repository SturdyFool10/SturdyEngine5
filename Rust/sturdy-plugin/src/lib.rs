//! Write a game module as a native plugin.
//!
//! A plugin is a `cdylib` the engine loads (`sturdy_plugin_load`). It holds one or more *modules*; modules from plugins written in any
//! language share one game, one frame and one ECS, and run as native code in the engine's parallel schedule. This crate is the safe
//! layer over the C ABI: [`Module`] is what you implement, [`Plugin`] is what the entry point describes, and [`sturdy_plugin!`]
//! exports the entry point.
//!
//! ```ignore
//! use sturdy_plugin::{sturdy_plugin, Engine, Module, ModuleOptions, Plugin};
//!
//! struct Hud;
//! impl Module for Hud {
//!     fn on_engine_initialized(&self, _engine: Engine) -> bool { true }
//! }
//!
//! sturdy_plugin!(|plugin: &mut Plugin| {
//!     plugin.set_info("rust", "my-plugin", env!("CARGO_PKG_VERSION"));
//!     plugin.add_module("hud", ModuleOptions::overlay_only().after(&["scene"]), Hud)?;
//!     Ok(())
//! });
//! ```
//!
//! Threading: `request_render_frame` may run on several per-window render threads at once, so [`Module`] requires `Send + Sync`
//! and the callbacks take `&self`. See `plans/threading-contract.md`. A panic never crosses the boundary: it is caught, reported to
//! stderr and the callback answers "failed" (or the process aborts for `on_shutdown`, which cannot report one).
//!
//! The engine's functions are declared in [`ffi`] and are *not linked* here: the plugin leaves them undefined and the loader resolves
//! them against the engine library already in the process. Build with the engine's shared library (`STURDY_BUILD_SHARED_LIBS=ON`).

/// The raw C ABI declarations (generated from `Sturdy.h`). They live in `sturdy-net`, which is built on the same declarations, so a plugin
/// that also uses [`net`] sees one set of types.
pub use sturdy_net::ffi;

/// Async networking, files, processes and data-parallel loops for plugins — the engine's Tokio and Rayon. See the `sturdy-net` crate.
pub use sturdy_net as net;
/// Scripts: units of game logic in any language (see `sturdy_script`).
pub use sturdy_script as script;

use core::ffi::{c_char, c_void};
use std::ffi::{CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};

pub use ffi::{SturdyEngine, SturdyFrame, SturdyFrameInput, SturdySurface};

/// A result code from the engine.
pub type Result<T = ()> = core::result::Result<T, Error>;

/// A failed C ABI call or an invalid description.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    /// The `SturdyResult` code (`ffi::STURDY_ERROR_*`).
    pub code: i32,
    pub message: String,
}

impl Error {
    pub fn new(code: i32, message: impl Into<String>) -> Self {
        Self { code, message: message.into() }
    }
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{} (code {})", self.message, self.code)
    }
}

impl std::error::Error for Error {}

/// The engine, valid only inside the callback that received it (the ABI revokes the handle afterwards; do not store it).
#[derive(Clone, Copy)]
pub struct Engine<'a> {
    raw: ffi::SturdyEngine,
    _scope: core::marker::PhantomData<&'a ()>,
}

impl Engine<'_> {
    /// The raw handle, for the `ffi::sturdy_*` functions.
    pub fn raw(&self) -> ffi::SturdyEngine {
        self.raw
    }
}

/// What a frame callback is asked about, valid only inside that callback.
#[derive(Clone, Copy)]
pub struct FrameRequest<'a> {
    pub engine: Engine<'a>,
    pub surface: ffi::SturdySurface,
    pub input: &'a ffi::SturdyFrameInput,
    /// Configure it with the `ffi::sturdy_frame_*` setters.
    pub frame: ffi::SturdyFrame,
}

impl FrameRequest<'_> {
    /// Asks the window this frame is for to close. The runtime then shuts the modules down in reverse start order.
    pub fn request_close(&self) -> Result {
        let result = unsafe { ffi::sturdy_window_request_close(self.engine.raw, self.surface, core::ptr::null_mut()) };
        if result == ffi::STURDY_OK { Ok(()) } else { Err(Error::new(result, "sturdy_window_request_close failed")) }
    }
}

/// A game module. Every callback has a do-nothing default.
pub trait Module: Send + Sync + 'static {
    /// Once, after the engine is up and before any frame, on the runtime thread. Return `false` to abort startup.
    fn on_engine_initialized(&self, _engine: Engine<'_>) -> bool {
        true
    }
    /// Once per frame per surface, possibly from a per-window render thread and possibly several at once. Return `true` to render
    /// the frame you configured, `false` to skip it. The default skips, which is right for a module that only runs systems: among
    /// scene modules the last one that returns `true` supplies the scene.
    fn request_render_frame(&self, _request: FrameRequest<'_>) -> bool {
        false
    }
    /// Once at teardown, on the runtime thread, before the engine goes away.
    fn on_shutdown(&self, _engine: Engine<'_>) {}
}

/// Where a module sits in the game.
#[derive(Clone, Debug, Default)]
pub struct ModuleOptions {
    after: Vec<String>,
    overlay_only: bool,
}

impl ModuleOptions {
    /// A module that may supply the scene (camera, lighting, render graph).
    pub fn scene() -> Self {
        Self::default()
    }
    /// A module that only draws overlay passes and never replaces the scene.
    pub fn overlay_only() -> Self {
        Self { after: Vec::new(), overlay_only: true }
    }
    /// Names of modules that must start before this one (and shut down after it).
    pub fn after(mut self, names: &[&str]) -> Self {
        self.after = names.iter().map(|n| (*n).to_owned()).collect();
        self
    }
}

struct Entry {
    module: Box<dyn Module>,
}

extern "C" fn trampoline_initialized(engine: ffi::SturdyEngine, user_data: *mut c_void) -> u8 {
    let entry = unsafe { &*(user_data as *const Entry) };
    guard("on_engine_initialized", false, || entry.module.on_engine_initialized(Engine { raw: engine, _scope: core::marker::PhantomData })) as u8
}

extern "C" fn trampoline_frame(
    engine: ffi::SturdyEngine,
    surface: ffi::SturdySurface,
    input: *const ffi::SturdyFrameInput,
    frame: ffi::SturdyFrame,
    user_data: *mut c_void,
) -> u8 {
    let entry = unsafe { &*(user_data as *const Entry) };
    guard("request_render_frame", false, || {
        entry.module.request_render_frame(FrameRequest {
            engine: Engine { raw: engine, _scope: core::marker::PhantomData },
            surface,
            input: unsafe { &*input },
            frame,
        })
    }) as u8
}

extern "C" fn trampoline_shutdown(engine: ffi::SturdyEngine, user_data: *mut c_void) {
    let entry = unsafe { &*(user_data as *const Entry) };
    // Shutdown cannot report failure; a panic here would leave the module half torn down, so it aborts.
    if catch_unwind(AssertUnwindSafe(|| entry.module.on_shutdown(Engine { raw: engine, _scope: core::marker::PhantomData }))).is_err() {
        eprintln!("sturdy-plugin: panic in on_shutdown; aborting");
        std::process::abort();
    }
}

extern "C" fn trampoline_destroy(user_data: *mut c_void) {
    drop(unsafe { Box::from_raw(user_data as *mut Entry) });
}

fn guard<T>(what: &str, on_panic: T, body: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(_) => {
            eprintln!("sturdy-plugin: panic in {what}; the callback reports failure");
            on_panic
        }
    }
}

/// The plugin being described by its entry point. Valid only inside the closure given to [`sturdy_plugin!`].
pub struct Plugin<'h> {
    host: &'h ffi::SturdyPluginHost,
}

/// Plain data a plugin hands to the next generation of itself across a reload, in one raw allocation that starts with its own layout
/// so it can be freed without knowing `T` (the unload hook does not). `T` is `Copy`: no pointers into this library (its code and
/// statics are gone after the reload) can be expressed safely, and there is no destructor to run.
#[repr(C)]
struct StateBox<T: Copy> {
    total_size: usize,
    total_align: usize,
    value_size: usize,
    value_align: usize,
    value: T,
}

/// Frees a `StateBox<_>` allocation of any `T`. Uses the system allocator, which every Rust library in the process shares by default.
unsafe fn free_state(state: *mut c_void) {
    #[repr(C)]
    struct Header {
        total_size: usize,
        total_align: usize,
    }
    let header = unsafe { &*(state as *const Header) };
    let layout = std::alloc::Layout::from_size_align(header.total_size, header.total_align).expect("a layout this crate wrote");
    unsafe { std::alloc::dealloc(state as *mut u8, layout) };
}

impl<'h> Plugin<'h> {
    /// Names the plugin and the language it is written in, for logs and `sturdy_plugin_language`; `version` is free-form.
    pub fn set_info(&mut self, language: &str, name: &str, version: &str) {
        let (language, name, version) = (cstring(language), cstring(name), cstring(version));
        unsafe { (self.host.set_info.expect("host table has set_info"))(self.host.context, language.as_ptr(), name.as_ptr(), version.as_ptr()) };
    }

    /// Adds a module. Its name must be unique within the plugin and across the game.
    pub fn add_module<M: Module>(&mut self, name: &str, options: ModuleOptions, module: M) -> Result {
        self.add_boxed(name, options, Box::new(module))
    }

    /// Adds a script (a [`script::ScriptModule`]) as a module of the game. Its name is the script's name.
    pub fn add_script(&mut self, mut script: script::ScriptModule) -> Result {
        let Some(mut descriptor) = script.take_raw() else {
            return Err(Error::new(ffi::STURDY_ERROR_INVALID_ARGUMENT, "the script module was already used"));
        };
        let result = unsafe { (self.host.add_module.expect("host table has add_module"))(self.host.context, &descriptor) };
        if result != ffi::STURDY_OK {
            // A refused module took no ownership.
            unsafe { ffi::sturdy_script_module_discard(&mut descriptor) };
            return Err(Error::new(result, "the engine refused the script module"));
        }
        Ok(())
    }

    /// [`add_module`](Self::add_module) for a module already boxed.
    pub fn add_boxed(&mut self, name: &str, options: ModuleOptions, module: Box<dyn Module>) -> Result {
        let name = CString::new(name).map_err(|_| Error::new(ffi::STURDY_ERROR_INVALID_ARGUMENT, "module name contains a NUL"))?;
        let after: Vec<CString> = options.after.iter().map(|a| cstring(a)).collect();
        let after_ptrs: Vec<*const c_char> = after.iter().map(|a| a.as_ptr()).collect();
        let user_data = Box::into_raw(Box::new(Entry { module })) as *mut c_void;

        let mut descriptor = ffi::SturdyGameModule::default();
        descriptor.struct_size = size_of::<ffi::SturdyGameModule>() as u32;
        descriptor.name = name.as_ptr();
        descriptor.after_count = after_ptrs.len() as u32;
        descriptor.after = if after_ptrs.is_empty() { core::ptr::null() } else { after_ptrs.as_ptr() };
        descriptor.contributes_scene = (!options.overlay_only) as u8;
        descriptor.logic.struct_size = size_of::<ffi::SturdyGameLogic>() as u32;
        descriptor.logic.user_data = user_data;
        descriptor.logic.on_engine_initialized = Some(trampoline_initialized);
        descriptor.logic.request_render_frame = Some(trampoline_frame);
        descriptor.logic.on_shutdown = Some(trampoline_shutdown);
        descriptor.logic.destroy = Some(trampoline_destroy);

        let result = unsafe { (self.host.add_module.expect("host table has add_module"))(self.host.context, &descriptor) };
        if result != ffi::STURDY_OK {
            // A refused module took no ownership.
            drop(unsafe { Box::from_raw(user_data as *mut Entry) });
            return Err(Error::new(result, "the engine refused the module"));
        }
        Ok(())
    }

    /// The state the previous generation of this plugin stored with [`set_state`](Self::set_state), if there was one of type `T`.
    /// Taking it moves ownership here: the previous generation's unload will not see it.
    pub fn take_prior_state<T: Copy>(&mut self) -> Option<T> {
        if self.host.prior_state.is_null() {
            return None;
        }
        // The header is read as raw words first: the stored type may differ from `T` if the plugin changed what it carries.
        let words = self.host.prior_state as *const usize;
        let matches = unsafe { *words.add(2) == size_of::<T>() && *words.add(3) == align_of::<T>() };
        let value = matches.then(|| unsafe { (*(self.host.prior_state as *const StateBox<T>)).value });
        unsafe { free_state(self.host.prior_state) };
        value
    }

    /// Stores plain data for the next generation of this plugin to read with [`take_prior_state`](Self::take_prior_state).
    pub fn set_state<T: Copy>(&mut self, value: T) {
        let layout = std::alloc::Layout::new::<StateBox<T>>();
        let state = unsafe { std::alloc::alloc(layout) } as *mut StateBox<T>;
        assert!(!state.is_null(), "out of memory storing plugin state");
        unsafe {
            state.write(StateBox { total_size: layout.size(), total_align: layout.align(), value_size: size_of::<T>(), value_align: align_of::<T>(), value });
            (self.host.set_state.expect("host table has set_state"))(self.host.context, state as *mut c_void);
        }
    }

    /// The path of the library being loaded (the shadow copy when the engine made one).
    pub fn path(&self) -> String {
        if self.host.path.is_null() {
            return String::new();
        }
        unsafe { CStr::from_ptr(self.host.path) }.to_string_lossy().into_owned()
    }
}

fn cstring(text: &str) -> CString {
    CString::new(text.replace('\0', "")).expect("NULs were removed")
}

/// The body of `sturdy_plugin_entry`; [`sturdy_plugin!`] calls this. Not part of the supported API.
#[doc(hidden)]
pub unsafe fn run_entry(
    host: *const ffi::SturdyPluginHost,
    reason: i32,
    state: *mut c_void,
    describe: fn(&mut Plugin<'_>) -> Result,
    unload: fn(Unload),
) -> i32 {
    let Some(host) = (unsafe { host.as_ref() }) else { return ffi::STURDY_ERROR_INVALID_ARGUMENT };
    if host.abi_major != ffi::ABI_VERSION_MAJOR {
        eprintln!("sturdy-plugin: built for ABI {} but the engine is {}", ffi::ABI_VERSION_MAJOR, host.abi_major);
        return ffi::STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE;
    }
    if reason == ffi::STURDY_PLUGIN_UNLOAD {
        // `Unload` frees whatever state is left when it drops, whether or not the hook looked at it.
        if catch_unwind(AssertUnwindSafe(|| unload(Unload { state }))).is_err() {
            eprintln!("sturdy-plugin: panic in the unload hook; continuing to unload");
        }
        return ffi::STURDY_OK;
    }
    match catch_unwind(AssertUnwindSafe(|| describe(&mut Plugin { host }))) {
        Ok(Ok(())) => ffi::STURDY_OK,
        Ok(Err(error)) => {
            eprintln!("sturdy-plugin: refusing to load: {error}");
            if error.code == ffi::STURDY_OK { ffi::STURDY_ERROR_INTERNAL } else { error.code }
        }
        Err(_) => {
            eprintln!("sturdy-plugin: panic while describing the plugin; refusing to load");
            ffi::STURDY_ERROR_INTERNAL
        }
    }
}

/// Handed to the unload hook: the engine is about to close the library, and every module this plugin added has been destroyed. Stop
/// anything the plugin started itself (threads, timers) here. State left unread is freed when this drops.
pub struct Unload {
    state: *mut c_void,
}

impl Unload {
    /// The state stored with [`Plugin::set_state`] and not handed on to a newer generation, if it is a `T`.
    pub fn take_state<T: Copy>(&mut self) -> Option<T> {
        if self.state.is_null() {
            return None;
        }
        let words = self.state as *const usize;
        let matches = unsafe { *words.add(2) == size_of::<T>() && *words.add(3) == align_of::<T>() };
        let value = matches.then(|| unsafe { (*(self.state as *const StateBox<T>)).value });
        unsafe { free_state(self.state) };
        self.state = core::ptr::null_mut();
        value
    }

    /// Whether the engine passed any state (false when a newer generation took it over).
    pub fn has_state(&self) -> bool {
        !self.state.is_null()
    }
}

impl Drop for Unload {
    fn drop(&mut self) {
        if !self.state.is_null() {
            unsafe { free_state(self.state) };
        }
    }
}

/// Exports `sturdy_plugin_entry`. The first argument is a `fn(&mut Plugin) -> Result` that runs on load and describes the plugin; the
/// optional `unload:` argument is a `fn(Unload)` that runs once before the library closes.
///
/// ```ignore
/// sturdy_plugin!(|plugin: &mut Plugin| { plugin.add_module("m", ModuleOptions::scene(), M)?; Ok(()) });
/// sturdy_plugin!(describe, unload: |unload| stop_my_threads());
/// ```
#[macro_export]
macro_rules! sturdy_plugin {
    ($describe:expr) => {
        $crate::sturdy_plugin!($describe, unload: |_unload: $crate::Unload| {});
    };
    ($describe:expr, unload: $unload:expr) => {
        /// The engine's entry point into this plugin.
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn sturdy_plugin_entry(
            host: *const $crate::ffi::SturdyPluginHost,
            reason: $crate::ffi::SturdyPluginReason,
            state: *mut ::core::ffi::c_void,
        ) -> i32 {
            // Coerced to plain function pointers here (non-capturing closures and fn items both qualify).
            let describe_fn: fn(&mut $crate::Plugin<'_>) -> $crate::Result = $describe;
            let unload_fn: fn($crate::Unload) = $unload;
            unsafe { $crate::run_entry(host, reason, state, describe_fn, unload_fn) }
        }
    };
}
